// veffbode: a Bode frequency shifter inside a delay's feedback loop, as an effect of the unit's own lists (level 8 of
// open-field's base, with level 7 for the values past the four knobs and level 10 for the page). It changes no place
// of the firmware and names no address of it.
//
// What the owner meets:
//   `veffbode` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer), and on every
//   wagon of fx train. It stands on the unit's `delay`: a sound saved with it keeps delay, switched off, elsewhere.
//   The four knobs (and the LFOs): SHIFT (-2000 .. +2000 Hz, the knob's distance from the middle cubed: fine near 0),
//   PHASE (0 .. 359 degrees of the oscillator), FEEDBACK (0 .. 90), MIX (0 .. 100).
//   With shift held, the four knobs turn the loop's values, kept with the sound (level 7, `veffbode_loop`):
//   DELAY (the notes 2/1 .. 1/64 of the tempo, then 0 ms, then 1 .. 1000 ms), SIDEBAND (down, both, up), TONE (the
//   loop's low-pass), SMEAR (all-passes in the loop); shift and a press of the 4th knob is CROSS (left and right
//   swapped in the loop). A press of the 1st knob puts the shift to 0, of the 2nd the phase to 0, of the 4th the mix.
//   It starts as a jet flanger, after Sonic Charge Echobode's preset of that name.
//
// The loop: in + feedback (a soft knee) -> delay -> smear -> tone (a low-pass, a high-pass at ~40 Hz) -> shifter
// (Hilbert pair, quadrature sine, the sidebands blended) -> out; the out is the feedback, crossed when CROSS.
// On the audio thread: no division, no 64-bit multiply, no float; every sum and difference held at full scale.

#include "build_r6e_card.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"                       // veff_ui.h (common, unedited)
#include "veff_ui.h"
#pragma GCC diagnostic pop
#include "bode_tables.h"

typedef unsigned int u32;
typedef int i32;
typedef short i16;

enum { ASK_EFFECT = 0, ASK_KEEPS = 1, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { CHANNELS = 2, BLOCK = 64, STAGES = 4 };
enum { V_SHIFT = 0, V_PHASE = 1, V_FEEDBACK = 2, V_MIX = 3 };
enum { E_DELAY = 0, E_SIDEBAND = 1, E_TONE = 2, E_SMEAR = 3, E_CROSS = 4, EXTRAS = 5 };
enum { LINE = 120000, SMEARS = 4, SMEAR_ROOM = 608 };                  // 2.72 s of delay; the all-passes' lines
enum { NOTES = 8, MS_STEPS = 85, DELAY_STEPS = NOTES + MS_STEPS };
enum { PLACES = 17 };                                                  // the sounds' slots 0..15 and the master
static const u32 BEATS_Q8[NOTES] = { 2048, 1024, 512, 256, 128, 64, 32, 16 };   // 2/1 .. 1/64 of a 4/4 bar, in beats * 256
static const char* const NOTE_NAMES[NOTES] = { "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64" };
static const int SMEAR_LEN[CHANNELS][SMEARS] = { { 142, 229, 379, 563 }, { 151, 241, 397, 601 } };
static const int EXTRA_START[EXTRAS] = { NOTES + 2, 85, 85, 15, 0 };   // the jet flanger: 2 ms, mostly up, mostly open, a little smear
static const int EXTRA_MOST[EXTRAS] = { DELAY_STEPS - 1, 100, 100, 100, 1 };
static const char KEY[] = "veffbode_loop";
static const char LOG_FILE[] = "op1f/veffbode.cpp";                   // the log shows "veffbode"

extern "C" const Op1fCard op1f_card_veffbode OP1F_NAMED("op1f_card_veffbode");

// ms of each step behind the notes: 0, 1..20, then 25..100 by 5, 110..300 by 10, 325..1000 by 25
static int step_ms(int s) {
    if (s <= 20) return s;
    if (s <= 36) return 20 + (s - 20) * 5;
    if (s <= 56) return 100 + (s - 36) * 10;
    return 300 + (s - 56) * 25;
}

#ifdef OP1F_HOST
static inline i32 mulq(i32 a, i32 b) {
    long long p = ((long long)a * b) >> 31;
    return p > 0x7fffffffLL ? 0x7fffffff : p < -0x7fffffffLL - 1 ? (i32)0x80000000 : (i32)p;
}
static inline i32 addq(i32 a, i32 b) {
    long long s = (long long)a + b;
    return s > 0x7fffffffLL ? 0x7fffffff : s < -0x7fffffffLL - 1 ? (i32)0x80000000 : (i32)s;
}
static inline i32 subq(i32 a, i32 b) {
    long long s = (long long)a - b;
    return s > 0x7fffffffLL ? 0x7fffffff : s < -0x7fffffffLL - 1 ? (i32)0x80000000 : (i32)s;
}
#else
static inline i32 mulq(i32 a, i32 b) { return __builtin_bfin_mult_fr1x32x32(a, b); }   // (a * b) >> 31, held
static inline i32 addq(i32 a, i32 b) { return __builtin_bfin_add_fr1x32(a, b); }
static inline i32 subq(i32 a, i32 b) { return __builtin_bfin_sub_fr1x32(a, b); }        // never -x: -(-1) does not exist
#endif

// ---------------------------------------------------------------- the values past the four knobs
// Kept by place (0..15 a sound's slot, 16 the master), written by the frame function, read by process: single words.
// A place run inside another pack (fx train's wagons) plays the start values.
static volatile int extra[PLACES][EXTRAS];
static volatile u32 extra_ready;                                       // bit n: place n's values were read

static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- the sound (audio thread)
struct Allpass { i32 x[2], y[2]; };                                    // in z^2: slot (frame & 1) holds the frame two back
struct Channel { Allpass a[STAGES], b[STAGES]; i32 a_late, last, lp, hp; };
struct State {
    Channel ch[CHANNELS];
    u32 phase, wpos, delay_q8, filled;                                 // filled: frames written since a fresh start
    i32 mix;
    u32 spos[CHANNELS][SMEARS];
    i32 smear[CHANNELS][SMEARS][SMEAR_ROOM];
    u32 dqs[BLOCK];                                                    // a chunk's scratch: the delay, the carrier,
    i32 csb[BLOCK], snb[BLOCK], ta[BLOCK], tb[BLOCK], wet[CHANNELS][BLOCK];   // the two Hilbert paths, the wet sides
    i16 line[CHANNELS][LINE];
};
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 501600, "the recipe's state_bytes");

static volatile u32 level;                                             // the last block's wet peak, for the page's combs
enum { SNAPS = 64 };                                                   // the page's wave: one input sample a call
static volatile i32 snap[SNAPS];
static volatile u32 snap_at;

static inline i32 soft(i32 x) { return subq(x, mulq(mulq(x, x), x) >> 2); }   // the feedback's knee: x - x^3/4

static inline i32 allpass(Allpass* s, i32 c, i32 x, u32 p) {
    const i32 y = mulq(c, x + s->y[p]) - s->x[p];
    s->x[p] = x; s->y[p] = y;
    return y;
}

static inline int held(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// SHIFT: -16383 .. 16383, cubed to -2000 .. +2000 Hz
static inline i32 shift_cube(int v, int* below) {
    int x = held(v, -16383, 16383);
    *below = x < 0;
    if (x < 0) x = -x;
    const i32 a = (i32)((u32)x << 17);
    return mulq(mulq(a, a), a);
}
static inline int shift_centihz(int v) { int b; const int c = mulq(shift_cube(v, &b), 200000); return b ? -c : c; }

static u32 delay_target_q8(int step, u32 period) {                    // the delay in frames * 256
    const int s = held(step, 0, DELAY_STEPS - 1);
    u32 q;
    if (s < NOTES) {
        u32 p = period >> 6;                                           // a 96th of a beat, frames * 2^12
        if (p > 40000000u) p = 40000000u;
        const u32 beat = (p * 96u) >> 12;                              // frames a beat
        q = beat * BEATS_Q8[s];
    } else q = (u32)step_ms(s - NOTES) * 11290u;                       // 44.1 frames a ms * 256
    const u32 most = (u32)(LINE - 4) << 8;
    return q > most ? most : q;
}

static void clear_small(State& st) {                                   // what a fresh start forgets: not the lines
    u32* w = (u32*)st.ch;
    for (u32 i = 0; i < sizeof(st.ch) / 4; ++i) w[i] = 0;
    u32* s = (u32*)st.smear;
    for (u32 i = 0; i < sizeof(st.smear) / 4; ++i) s[i] = 0;
    st.filled = 0;
    st.mix = 0;
}

#define SMEAR_LEN_C(c, k) ((c) ? (k) == 0 ? 151 : (k) == 1 ? 241 : (k) == 2 ? 397 : 601 : (k) == 0 ? 142 : (k) == 1 ? 229 : (k) == 2 ? 379 : 563)
static_assert(SMEAR_LEN_C(0, 0) == 142 && SMEAR_LEN_C(0, 3) == 563 && SMEAR_LEN_C(1, 0) == 151 && SMEAR_LEN_C(1, 3) == 601, "as SMEAR_LEN");
// The smear's gain is at most 0.7: a 16-bit product (the sample's top half times g in Q15) is plenty.
static inline i32 mulg(i32 x, i32 g16) {
#ifdef OP1F_HOST
    return (int)((x >> 16) * g16) * 2;
#else
    return __builtin_bfin_mult_fr1x32((short)(x >> 16), (short)g16);
#endif
}
template <int LEN> static inline __attribute__((always_inline)) i32 smear_stage(i32* buf, u32& at, i32 y, i32 g16) {
    const i32 vv = buf[at];
    const i32 w = addq(y, mulg(vv, g16));
    buf[at] = w;
    at = at + 1 >= (u32)LEN ? 0 : at + 1;
    return subq(vv, mulg(w, g16));
}

// One side of one frame: the line, the smear, the tone and the shifter. Inlined into the frame loop; the filters'
// state is the block's local copy (process copies it in and back), so the loop touches the working memory only for
// the lines and the smear's buffers.
template <u32 P, int C> static inline __attribute__((always_inline)) i32 side(Channel& ch, i16* line, i32 (*smear)[SMEAR_ROOM], u32* spos,
                                                      i32 x, u32 wpos, u32 whole, u32 frac, u32 filled,
                                                      i32 g, i32 tone, i32 cs, i32 sn, i32 sb) {
    line[wpos] = (i16)(x >> 16);
    i32 y = 0;
    if (whole + 1u <= filled) {                                        // older than a fresh start is silence
        int r0 = (int)wpos - (int)whole; if (r0 < 0) r0 += LINE;
        const int r1 = r0 == 0 ? LINE - 1 : r0 - 1;
        const i32 a0 = line[r0], a1 = line[r1];
        y = (a0 * 256 + (a1 - a0) * (i32)frac) * 256;                  // in 256ths of a sample first: no overflow
    } else if (whole == 0u) y = (i32)line[wpos] * 65536;
    if (g) {                                                           // smear: two all-pass delays; at 0 none at all
        y = smear_stage<SMEAR_LEN_C(C, 2)>(smear[2], spos[2], y, g);
        y = smear_stage<SMEAR_LEN_C(C, 3)>(smear[3], spos[3], y, g);
    }
    ch.lp = addq(ch.lp, mulq(subq(y, ch.lp), tone));
    ch.hp = addq(ch.hp, mulq(subq(ch.lp, ch.hp), 12240000));
    const i32 fx = subq(ch.lp, ch.hp) >> 3;                            // Q28 into the shifter
    i32 a = fx, b = fx;
    for (int j = 0; j < STAGES; ++j) a = allpass(&ch.a[j], HILBERT_A[j], a, P);
    for (int j = 0; j < STAGES; ++j) b = allpass(&ch.b[j], HILBERT_B[j], b, P);
    const i32 re = ch.a_late;
    ch.a_late = a;
    const i32 o = mulq(re, cs) + mulq(mulq(b, sn), sb);                // the sidebands blended
    return o > 0x0fffffff ? 0x7fffffff : o < -0x10000000 ? (i32)0x80000000 : o * 8;
}

// A chunk: frames whose every read of the line is older than the chunk (the delay longer than the chunk), so each
// stage runs over the whole chunk with its state in registers, one stage after another; the same arithmetic in the
// same order for each filter as frame by frame, so the same sound to the bit. The loop's feedback goes into the line
// last, frame by frame, from the wet of the frame before.
static inline void ap_pass(Allpass& s, i32 c, const i32* in, i32* out, u32 k, u32 p0) {
    i32 x2 = s.x[p0], x1 = s.x[p0 ^ 1u], y2 = s.y[p0], y1 = s.y[p0 ^ 1u];
    for (u32 j = 0; j < k; ++j) {
        const i32 x = in[j];
        const i32 y = mulq(c, x + y2) - x2;
        out[j] = y;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
    }
    const u32 pk = (p0 + k) & 1u;
    s.x[pk] = x2; s.x[pk ^ 1u] = x1; s.y[pk] = y2; s.y[pk ^ 1u] = y1;
}

template <int LEN> static inline void smear_pass(i32* buf, u32& spos, i32* io, u32 k, i32 g16) {
    u32 at = spos;
    for (u32 j = 0; j < k; ++j) io[j] = smear_stage<LEN>(buf, at, io[j], g16);
    spos = at;
}

template <int C> static void chunk_side(State& st, Channel& ch, u32* spos, u32 k, u32 wpos, u32 filled, i32 g, i32 tone, i32 sb) {
    const i16* line = st.line[C];
    i32* const ta = st.ta;
    i32* const tb = st.tb;
    for (u32 j = 0; j < k; ++j) {
        const u32 d = st.dqs[j], whole = d >> 8, frac = d & 255u;
        const u32 fl = filled + j < (u32)LINE ? filled + j : (u32)LINE;
        i32 y = 0;
        if (whole + 1u <= fl) {                                        // older than a fresh start is silence
            int w = (int)wpos + (int)j; if (w >= LINE) w -= LINE;
            int r0 = w - (int)whole; if (r0 < 0) r0 += LINE;
            const int r1 = r0 == 0 ? LINE - 1 : r0 - 1;
            const i32 a0 = line[r0], a1 = line[r1];
            y = (a0 * 256 + (a1 - a0) * (i32)frac) * 256;
        }
        ta[j] = y;
    }
    if (g) {
        smear_pass<SMEAR_LEN_C(C, 2)>(st.smear[C][2], spos[2], ta, k, g);
        smear_pass<SMEAR_LEN_C(C, 3)>(st.smear[C][3], spos[3], ta, k, g);
    }
    i32 lp = ch.lp, hp = ch.hp;
    for (u32 j = 0; j < k; ++j) {
        lp = addq(lp, mulq(subq(ta[j], lp), tone));
        hp = addq(hp, mulq(subq(lp, hp), 12240000));
        ta[j] = subq(lp, hp) >> 3;                                     // Q28 into the shifter
    }
    ch.lp = lp; ch.hp = hp;
    const u32 p0 = wpos & 1u;
    ap_pass(ch.b[0], HILBERT_B[0], ta, tb, k, p0);
    for (int j = 1; j < STAGES; ++j) ap_pass(ch.b[j], HILBERT_B[j], tb, tb, k, p0);
    for (int j = 0; j < STAGES; ++j) ap_pass(ch.a[j], HILBERT_A[j], ta, ta, k, p0);
    i32 re = ch.a_late;
    i32* const wet = st.wet[C];
    for (u32 j = 0; j < k; ++j) {
        const i32 o = mulq(re, st.csb[j]) + mulq(mulq(tb[j], st.snb[j]), sb);   // the sidebands blended
        re = ta[j];
        wet[j] = o > 0x0fffffff ? 0x7fffffff : o < -0x10000000 ? (i32)0x80000000 : o * 8;
    }
    ch.a_late = re;
}

extern "C" void veffbode_process(const Op1fEffectBlock* now) OP1F_NAMED("veffbode_process");
extern "C" void veffbode_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    if (now->fresh) clear_small(st);
    { const u32 at = snap_at; snap[at & (SNAPS - 1)] = now->left[0]; snap_at = at + 1; }   // for the page only
    const int* v = now->values;
    const volatile int* e = extras_of(now->place);
    const int delay_step = e ? e[E_DELAY] : EXTRA_START[E_DELAY], sidev = e ? e[E_SIDEBAND] : EXTRA_START[E_SIDEBAND];
    const int tone_pct = e ? e[E_TONE] : EXTRA_START[E_TONE], smear_pct = e ? e[E_SMEAR] : EXTRA_START[E_SMEAR];
    const int cross = e ? e[E_CROSS] & 1 : 0;
    int below;
    const u32 inc = (u32)mulq(shift_cube(v[V_SHIFT], &below), 194784000);   // 2000 Hz * 2^32 / 44100
    const u32 turn = (u32)held(v[V_PHASE], 0, 359) * 11930465u;        // degrees to 2^32 / 360
    const i32 fb = held(v[V_FEEDBACK], 0, 90) * 21474836;              // hundredths in Q31
    const i32 want = held(v[V_MIX], 0, 100) >= 100 ? 0x7fffffff : held(v[V_MIX], 0, 100) * 21474836;
    const i32 sb = (held(sidev, 0, 100) - 50) * 42949672;              // -1 .. +1: 0 down, 50 both, 100 up
    const i32 t = held(tone_pct, 0, 100) >= 100 ? 0x7fffffff : held(tone_pct, 0, 100) * 21474836;
    const i32 tone = 0x02000000 + mulq(t, t) - mulq(0x02000000, t);   // the low-pass's step: ~100 Hz .. open
    const i32 g = mulq(held(smear_pct, 0, 100) * 21474836, 0x5999999a) >> 16;   // smear: 0 .. 0.7, Q15
    const u32 target = delay_target_q8(delay_step, now->period);
    const i32 from = st.mix;
    if (!now->on || (want == 0 && from == 0)) return;                  // off, or mix 0: the sound as it came
    const i32 step = (want >> 6) - (from >> 6);
    int* const left = now->left;
    int* const right = now->right;
    const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    Channel ch[CHANNELS] = { st.ch[0], st.ch[1] };                     // the filters, local for the block (288 bytes)
    u32 spos[CHANNELS][SMEARS];
    for (int c = 0; c < CHANNELS; ++c) for (int k = 0; k < SMEARS; ++k) spos[c][k] = st.spos[c][k];
    u32 phase = st.phase, wpos = st.wpos, dq = st.delay_q8, filled = st.filled;
    const u32 pinc = below ? (u32)0 - inc : inc;                       // below 0 the oscillator runs backward
    i32 m = from, peak = 0;
#define VEFFBODE_FRAME(PAR) do { \
        dq += (u32)(((i32)target - (i32)dq) >> 9); \
        const u32 whole = dq >> 8, frac = dq & 255u; \
        const u32 ph = phase + turn, i = ph >> 22; \
        const i32 f = (i32)((ph >> 6) & 0xffffu) << 15; \
        const i32 s0 = SINE[i], s1 = SINE[i + 1], c0 = SINE[(i + 256u) & 1023u], c1 = SINE[((i + 256u) & 1023u) + 1]; \
        const i32 sn = s0 + mulq((s1 >> 1) - (s0 >> 1), f) * 2, cs = c0 + mulq((c1 >> 1) - (c0 >> 1), f) * 2; \
        m += step; \
        const i32 dl = left[n], dr = right[n]; \
        const i32 xl = addq(dl, mulq(fb, soft(ch[cross].last))); \
        const i32 xr = addq(dr, mulq(fb, soft(ch[1 - cross].last))); \
        const i32 wl = side<PAR, 0>(ch[0], st.line[0], st.smear[0], spos[0], xl, wpos, whole, frac, filled, g, tone, cs, sn, sb); \
        const i32 wr = side<PAR, 1>(ch[1], st.line[1], st.smear[1], spos[1], xr, wpos, whole, frac, filled, g, tone, cs, sn, sb); \
        ch[0].last = wl; ch[1].last = wr; \
        const i32 keep = 0x7fffffff - m; \
        left[n] = addq(mulq(keep, dl), mulq(m, wl)); \
        right[n] = addq(mulq(keep, dr), mulq(m, wr)); \
        const i32 ml = wl < 0 ? (wl == (i32)0x80000000 ? 0x7fffffff : -wl) : wl; \
        const i32 mr = wr < 0 ? (wr == (i32)0x80000000 ? 0x7fffffff : -wr) : wr; \
        if (ml > peak) peak = ml; \
        if (mr > peak) peak = mr; \
        phase += pinc; \
        if (++wpos >= (u32)LINE) wpos = 0; \
        if (filled < (u32)LINE) ++filled; \
} while (0)
    u32 n = 0;
    while (n < frames) {
        u32 k = 0, d = dq;                                             // the chunk: as long as every read is older
        while (k < frames - n) {
            const u32 nd = d + (u32)(((i32)target - (i32)d) >> 9);
            if ((nd >> 8) < k + 1u) break;
            st.dqs[k] = nd; d = nd; ++k;
        }
        if (k < 2u) {                                                  // a delay shorter than two frames: frame by frame
            if (wpos & 1u) VEFFBODE_FRAME(1u); else VEFFBODE_FRAME(0u);
            ++n;
            continue;
        }
        for (u32 j = 0; j < k; ++j) {                                  // the carrier of each frame
            const u32 ph = phase + j * pinc + turn, i = ph >> 22;
            const i32 f = (i32)((ph >> 6) & 0xffffu) << 15;
            const i32 s0 = SINE[i], s1 = SINE[i + 1], c0 = SINE[(i + 256u) & 1023u], c1 = SINE[((i + 256u) & 1023u) + 1];
            st.snb[j] = s0 + mulq((s1 >> 1) - (s0 >> 1), f) * 2;
            st.csb[j] = c0 + mulq((c1 >> 1) - (c0 >> 1), f) * 2;
        }
        chunk_side<0>(st, ch[0], spos[0], k, wpos, filled, g, tone, sb);
        chunk_side<1>(st, ch[1], spos[1], k, wpos, filled, g, tone, sb);
        const i32* const wa = st.wet[cross];                           // the feedback's source for the left, crossed or not
        const i32* const wb = st.wet[1 - cross];
        i32 pa = ch[cross].last, pb = ch[1 - cross].last;
        u32 w = wpos;
        for (u32 j = 0; j < k; ++j, ++n) {
            m += step;
            const i32 dl = left[n], dr = right[n];
            st.line[0][w] = (i16)(addq(dl, mulq(fb, soft(pa))) >> 16);
            st.line[1][w] = (i16)(addq(dr, mulq(fb, soft(pb))) >> 16);
            pa = wa[j]; pb = wb[j];
            if (++w >= (u32)LINE) w = 0;
            const i32 wl = st.wet[0][j], wr = st.wet[1][j];
            const i32 keep = 0x7fffffff - m;
            left[n] = addq(mulq(keep, dl), mulq(m, wl));
            right[n] = addq(mulq(keep, dr), mulq(m, wr));
            const i32 ml = wl < 0 ? (wl == (i32)0x80000000 ? 0x7fffffff : -wl) : wl;
            const i32 mr = wr < 0 ? (wr == (i32)0x80000000 ? 0x7fffffff : -wr) : wr;
            if (ml > peak) peak = ml;
            if (mr > peak) peak = mr;
        }
        ch[0].last = st.wet[0][k - 1]; ch[1].last = st.wet[1][k - 1];
        wpos = w;
        phase += k * pinc;
        dq = st.dqs[k - 1];
        filled = filled + k < (u32)LINE ? filled + k : (u32)LINE;
    }
#undef VEFFBODE_FRAME
    st.ch[0] = ch[0]; st.ch[1] = ch[1];
    for (int c = 0; c < CHANNELS; ++c) for (int k = 0; k < SMEARS; ++k) st.spos[c][k] = spos[c][k];
    st.phase = phase; st.wpos = wpos; st.delay_q8 = dq; st.filled = filled;
    st.mix = want;
    level = (u32)peak;
}

extern "C" void veffbode_reset(unsigned place, void* state) OP1F_NAMED("veffbode_reset");
extern "C" void veffbode_reset(unsigned place, void* state) {
    (void)place;
    if (state == 0) return;
    // everything but the two lines (~15 KB, not 500 KB): with filled 0 the lines are never read before written, so
    // they need no clearing; clearing them all coincided with the base switching the effect off (6M cycles)
    u32* w = (u32*)state;
    const u32 words = (u32)((const char*)&((State*)state)->line[0][0] - (const char*)state) >> 2;
    for (u32 i = 0; i < words; ++i) w[i] = 0;
}

// ---------------------------------------------------------------- told by the base (display thread): single words
static volatile u32 shown_where, shown_how;
static volatile u32 reread;                                            // bit n: read place n's kept values again

extern "C" void veffbode_shown(unsigned where, unsigned how) OP1F_NAMED("veffbode_shown");
extern "C" void veffbode_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffbode_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffbode_changed");
extern "C" void veffbode_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread): single words
static volatile int pending_turn[4];                                   // shift-held turns of the four knobs, in detents
static volatile u32 pending_click;                                     // bit k: knob k pressed; bit 4: cross

extern "C" int veffbode_input(const Op1fInput* e) OP1F_NAMED("veffbode_input");
extern "C" int veffbode_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {         // shift held: the loop's values are ours
        const int steps = e->turn >> 10;
        if (steps) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        if (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) { if (knob == 3) pending_click |= 16u; }
        else if (knob != 2) pending_click |= 1u << knob;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)PRESS_FIRST && e->key <= (u32)PRESS_LAST) return 1;
    return 0;                                                          // the knobs' turns go to the unit: it moves our values
}

// ---------------------------------------------------------------- the frame (display thread)
static char* put(char* p, const char* s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char* put_number(char* p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char d[12]; int k = 0; u32 n = (u32)v;
    do { u32 tens = 0; while (n >= 10u) { n -= 10u; ++tens; } d[k++] = (char)('0' + n); n = tens; } while (n && k < 11);
    while (k) *p++ = d[--k];
    *p = 0; return p;
}

static void extras_start(u32 p) { for (int k = 0; k < EXTRAS; ++k) extra[p][k] = EXTRA_START[k]; }

// Our own form only: {"d":10,"s":85,"t":85,"m":15,"x":0,"v":1}
static int read_member(const char* t, char name, int* out) {
    for (; *t; ++t) {
        if (t[0] == '"' && t[1] == name && t[2] == '"') {
            const char* q = t + 3;
            while (*q == ' ' || *q == ':') ++q;
            int sign = 1, v = 0, any = 0;
            if (*q == '-') { sign = -1; ++q; }
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); ++q; any = 1; }
            if (any) { *out = sign * v; return 1; }
            return 0;
        }
    }
    return 0;
}

static void where_of(u32 p, unsigned* where, unsigned* which) {
    if (p < 8u) { *where = OP1F_KEEP_SYNTH; *which = p; }
    else if (p < 16u) { *where = OP1F_KEEP_DRUM; *which = p - 8u; }
    else { *where = OP1F_KEEP_MASTER; *which = 0; }
}

static void extras_read(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffbode, where, which, KEY, text, sizeof text) > 0) {
        static const char NAMES[EXTRAS] = { 'd', 's', 't', 'm', 'x' };
        for (int k = 0; k < EXTRAS; ++k) { int v; if (read_member(text, NAMES[k], &v)) extra[p][k] = held(v, 0, EXTRA_MOST[k]); }
    }
    extra_ready |= 1u << p;
}

static u32 unsaved;                                                    // bit n: place n's values to be written

static void extras_write(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96], * q = text;
    q = put(q, "{\"d\":"); q = put_number(q, extra[p][E_DELAY]);
    q = put(q, ",\"m\":"); q = put_number(q, extra[p][E_SMEAR]);
    q = put(q, ",\"s\":"); q = put_number(q, extra[p][E_SIDEBAND]);
    q = put(q, ",\"t\":"); q = put_number(q, extra[p][E_TONE]);
    q = put(q, ",\"v\":1,\"x\":"); q = put_number(q, extra[p][E_CROSS]); put(q, "}");
    if (op1f_value_set(&op1f_card_veffbode, where, which, KEY, text) == 0) unsaved &= ~(1u << p);   // else again next frame
}

// ---------------------------------------------------------------- the page: the common Veff page (veff_ui.h)
// Painted every frame on the base's blank page. The centre picture, after the Veff reference sheet: four bell-shaped
// sines crossing along the axis (two white, two pink), two pink circles overlapping at the middle with a filled lens,
// a dashed centre line and sparkles. The circles part and orbit with the shift; the waves scroll with the shift and
// the phase; their height follows the input's envelope; feedback and mix set the brightness. Integer maths only.
static char* put_hundredths(char* p, int c) {                          // +5.25, +12.5, +120
    p = put(p, c < 0 ? "-" : "+");
    u32 n = (u32)(c < 0 ? -c : c), whole = 0;
    while (n >= 100u) { n -= 100u; ++whole; }
    p = put_number(p, (int)whole);
    if (whole >= 100u) return p;
    if (whole >= 10u) { u32 tenths = 0; while (n >= 10u) { n -= 10u; ++tenths; } p = put(p, "."); return put_number(p, (int)tenths); }
    p = put(p, n < 10u ? ".0" : ".");
    return put_number(p, (int)n);
}

static char* put_shift(char* p, int centihz) {                         // +120 Hz, +5.25 Hz, -1.62 kHz
    const int a = centihz < 0 ? -centihz : centihz;
    if (a < 100000) { p = put_hundredths(p, centihz); return put(p, " Hz"); }
    int tens = 0; u32 n = (u32)a;                                       // in tens of Hz: 1.62 kHz is 162
    while (n >= 1000u) { n -= 1000u; ++tens; }
    p = put_hundredths(p, centihz < 0 ? -tens : tens);
    return put(p, " kHz");
}

static struct { u32 asked, theta, tick; int env, lvl; char line[4][16]; } f;

enum { WAVE_LEFT = 24, WAVE_RIGHT = 456, WAVE_PTS = 32, LOBE = 40 };

static void paint_art(void* c, const int* v, const int* x, int hz) {
    using namespace veffui;
    // the envelope: the input snapshot's peak, eased (0..256)
    const u32 at = snap_at;
    int pk = 0;
    for (int i = 0; i < SNAPS; ++i) {
        const int s = snap[(at - (u32)SNAPS + (u32)i) & (SNAPS - 1)] >> 16;
        const int a = s < 0 ? -s : s;
        if (a > pk) pk = a;
    }
    int target = (pk * 3) >> 7;
    if (target > 256) target = 256;
    f.env += (target - f.env) >> 2;
    const int now = (int)(level >> 20);
    f.lvl = now > f.lvl ? now : f.lvl - (f.lvl >> 3);
    ++f.tick;
    f.theta += (u32)((hz * 13) >> 9);                                  // the shift's speed, 1024ths of a turn << 8
    const int phase = (held(v[V_PHASE], 0, 359) * 182) >> 8;           // in 256ths of a turn
    const int scroll = (int)(f.theta >> 10) + phase;                   // in 256ths
    const int mix = held(v[V_MIX], 0, 100), fb = held(v[V_FEEDBACK], 0, 90);
    const int bright = 96 + ((mix * 410) >> 8);                        // 96..256
    const int echo = 64 + ((fb * 546) >> 8);                          // 64..256: the second pair, the feedback's
    const int sign = x[E_SIDEBAND] < 34 ? -1 : 1;

    // the axis and the dashed centre
    axis(c);

    // the two circles: apart with the shift (its octaves), turning about the middle at its speed
    int a = hz < 0 ? -hz : hz, apart = 14;
    while (a > 25 && apart < 28) { a >>= 1; apart += 2; }
    apart += x[E_CROSS] ? 6 : 0;
    const int turn = (int)(f.theta >> 10);
    const int ox = (cos256(turn) * apart) >> 14, oy = (sin256(turn) * apart) >> 16;   // flattened orbit
    const int r = 30 + ((f.env * 10) >> 8);                             // 30..40, + the orbit's 7: within VEFF_ART_HALF
    const int c1x = VEFF_MID_X - ox, c1y = VEFF_AXIS_Y - oy, c2x = VEFF_MID_X + ox, c2y = VEFF_AXIS_Y + oy;
    // the lens: the rows both circles cover, pink, brighter with the wet level
    const int lens = held(((bright * (128 + held(f.lvl * 2, 0, 128))) >> 8), 40, 230);
    for (int y = VEFF_AXIS_Y - r; y <= VEFF_AXIS_Y + r; y += 1) {
        const int d1 = y - c1y, d2 = y - c2y;
        if (d1 < -r || d1 > r || d2 < -r || d2 > r) continue;
        const int w1 = isqrt(r * r - d1 * d1), w2 = isqrt(r * r - d2 * d2);
        const int l = (c1x - w1) > (c2x - w2) ? c1x - w1 : c2x - w2, rr = (c1x + w1) < (c2x + w2) ? c1x + w1 : c2x + w2;
        if (rr > l) op1f_paint_box(c, l, y, rr - l + 1, 1, pink(lens));
    }
    // the circles
    ring(c, c1x, c1y, r, r, pink(bright), 1, 5);
    ring(c, c2x, c2y, r, r, pink(bright), 1, 5);

    // the waves: sin(2 turns across - scroll) under a bell; white the input's pair, pink the shifted pair
    const int amp = 26 + ((f.env * 24) >> 8);                          // 26..50: VEFF_ART_HALF at full
    for (int wv = 0; wv < 4; ++wv) {
        const int pinkish = wv & 1, second = wv >> 1;
        const int off = pinkish ? 128 + sign * (scroll >> 1) : 0;      // the pink pair against the white: the shift
        const int h = second ? amp >> 1 : amp;
        const int alpha = (bright * (second ? echo : 256)) >> 8;
        const unsigned col = pinkish ? pink(alpha) : white(alpha);
        int px = 0, py = 0;
        for (int i = 0; i <= WAVE_PTS; ++i) {
            const int t = i << 3;                                      // 0..256 across, WAVE_PTS 32
            const int bell = (sin256(t >> 1) * sin256(t >> 1)) >> 14;  // sin^2, Q14
            const int s = sin256(t * 2 - scroll + off + second * 64);
            const int qx = WAVE_LEFT + (((WAVE_RIGHT - WAVE_LEFT) * t) >> 8);
            const int qy = VEFF_AXIS_Y - ((((s * bell) >> 14) * h) >> 14);
            if (i) op1f_paint_line(c, px, py, qx, qy, 1, col);
            px = qx; py = qy;
        }
    }
    dashed_v(c, VEFF_MID_X, VEFF_AXIS_Y - VEFF_ART_HALF, VEFF_AXIS_Y + VEFF_ART_HALF, VEFF_WHITE, 4, 3);

    sparkles(c);
}

static void paint(void* canvas, u32 where, int shift, int on) {
    int v[4];
    for (u32 i = 0; i < 4u; ++i) if (op1f_param_heard(&op1f_card_veffbode, ASK_EFFECT, where, i, &v[i]) != 0) v[i] = 0;
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
    const int hz = shift_centihz(v[V_SHIFT]);
    veffui::Column col[4];
    char (*t)[16] = f.line;
    if (!shift) {
        put_shift(t[0], hz);
        put_number(t[1], held(v[V_PHASE], 0, 359));
        put(put_number(t[2], held(v[V_FEEDBACK], 0, 90)), "%");
        put(put_number(t[3], held(v[V_MIX], 0, 100)), "%");
        col[0] = { "SHIFT", t[0], (held(v[V_SHIFT], -16383, 16383) + 16384) >> 5 };
        col[1] = { "PHASE", t[1], (held(v[V_PHASE], 0, 359) * 2921) >> 10 };
        col[2] = { "FEEDBACK", t[2], (held(v[V_FEEDBACK], 0, 90) * 11651) >> 10 };
        col[3] = { "MIX", t[3], (held(v[V_MIX], 0, 100) * 10486) >> 10 };
    } else {
        const int st = held(x[E_DELAY], 0, DELAY_STEPS - 1);
        if (st < NOTES) put(t[0], NOTE_NAMES[st]); else put(put_number(t[0], step_ms(st - NOTES)), " ms");
        put(t[1], x[E_SIDEBAND] < 34 ? "down" : x[E_SIDEBAND] > 66 ? "up" : "both");
        put(put_number(t[2], x[E_TONE]), "%");
        put(put_number(t[3], x[E_SMEAR]), x[E_CROSS] ? "% x" : "%");
        col[0] = { "DELAY", t[0], (st * 11398) >> 10 };
        col[1] = { "SIDEBAND", t[1], (x[E_SIDEBAND] * 10486) >> 10 };
        col[2] = { "TONE", t[2], (x[E_TONE] * 10486) >> 10 };
        col[3] = { x[E_CROSS] ? "SMEAR CROSS" : "SMEAR", t[3], (x[E_SMEAR] * 10486) >> 10 };
    }
    paint_art(canvas, v, x, hz);
    veffui::page(canvas, "BODE", veffui::ICON_BODE, col, shift ? "SHIFT" : 0, on);
}

extern "C" void veffbode_frame(const Op1fFrame* now) OP1F_NAMED("veffbode_frame");
extern "C" void veffbode_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffbode, "veffbodepage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffbode, ASK_EFFECT, page_doc);
    }
    if (!f.asked) {
        f.asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
    }
    // the kept values: read again where the base said they changed, or the first time a place rides us
    const u32 riding = (u32)op1f_riding(&op1f_card_veffbode, ASK_EFFECT);
    u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    // the page's own keys: shift-held turns and presses
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        static const int STEP[EXTRAS] = { 1, 2, 2, 2, 1 };
        for (int k = 0; k < 4; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            extra[where][k] = held(extra[where][k] + t * STEP[k], 0, EXTRA_MOST[k]);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 16u) { extra[where][E_CROSS] ^= 1; unsaved |= 1u << where; }
        if (clicks & 1u) op1f_param_set(&op1f_card_veffbode, ASK_EFFECT, where, V_SHIFT, 0);
        if (clicks & 2u) op1f_param_set(&op1f_card_veffbode, ASK_EFFECT, where, V_PHASE, 0);
        if (clicks & 8u) op1f_param_set(&op1f_card_veffbode, ASK_EFFECT, where, V_MIX, 0);
    } else {
        for (int k = 0; k < 4; ++k) pending_turn[k] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffbode && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP))
        paint(now->canvas, where, now->shift != 0, !(how & (u32)OP1F_SHOWN_OFF));
}
