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
    u32* w = (u32*)state;
    for (u32 i = 0; i < (u32)(STATE_BYTES >> 2); ++i) w[i] = 0;
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

// ---- the faceplate: veffbode.svg (veff/veffbode/face.py), every letter drawn by the picture itself
enum { WAVE_FRAMES = 12, SPHERE_FRAMES = 8, ARC_STEPS = 16, SIDE_ICONS = 3, PLACES_TEXT = 8 };
static const char GLYPHS[] = "0123456789+-.%/mshzupdownbtx";        // as face.py's GLYPHS
enum { GLYPH_COUNT = sizeof(GLYPHS) - 1 };

static struct {
    u32 asked, stood, wave_acc, sphere_acc, lvl, said;
    void* doc;
    int wave, sphere, side, arcs[4], comb_dx, comb_sy, page;
    void* waves[WAVE_FRAMES]; void* spheres[SPHERE_FRAMES]; void* sides[SIDE_ICONS]; void* arc[4][ARC_STEPS + 1];
    void* comb_l; void* comb_r; void* mark[2][2]; void* labels[4][2];
    void* glyph[4][PLACES_TEXT][GLYPH_COUNT]; signed char shown_glyph[4][PLACES_TEXT];
    char line[24];
} f;

static void show(void* n, int on) { if (n) op1f_show(n, on); }

static char* name_of(char* q, const char* a, int n, const char* b, int m) {
    char* p = put(q, a); p = put_number(p, n);
    if (b) { p = put(p, b); put_number(p, m); }
    return q;
}

static void make_page() {
    void* d = op1f_picture(&op1f_card_veffbode, "veffbodepage.svg");
    char q[24];
    if (d) {
        for (int i = 0; i < WAVE_FRAMES; ++i) { f.waves[i] = op1f_node(d, name_of(q, "bw_", i, 0, 0)); show(f.waves[i], 0); }
        for (int i = 0; i < SPHERE_FRAMES; ++i) { f.spheres[i] = op1f_node(d, name_of(q, "bs_", i, 0, 0)); show(f.spheres[i], 0); }
        for (int i = 0; i < SIDE_ICONS; ++i) { f.sides[i] = op1f_node(d, name_of(q, "bsd", i, 0, 0)); show(f.sides[i], 0); }
        for (int c = 0; c < 4; ++c) {
            for (int s = 1; s <= ARC_STEPS; ++s) { f.arc[c][s] = op1f_node(d, name_of(q, "ba", c, "_", s)); show(f.arc[c][s], 0); }
            for (int pg = 0; pg < 2; ++pg) { f.labels[c][pg] = op1f_node(d, name_of(q, "bl", c, "_", pg)); show(f.labels[c][pg], 0); }
            for (int i = 0; i < PLACES_TEXT; ++i) {
                f.shown_glyph[c][i] = -1;
                for (int g = 0; g < GLYPH_COUNT; ++g) {
                    q[0] = 't'; q[1] = (char)('0' + c); q[2] = (char)('0' + i); q[3] = '_'; put_number(q + 4, g);
                    f.glyph[c][i][g] = op1f_node(d, q); show(f.glyph[c][i][g], 0);
                }
            }
        }
        f.comb_l = op1f_node(d, "bcl"); f.comb_r = op1f_node(d, "bcr");
        f.mark[0][0] = op1f_node(d, "bml_off"); f.mark[0][1] = op1f_node(d, "bml_on");
        f.mark[1][0] = op1f_node(d, "bmr_off"); f.mark[1][1] = op1f_node(d, "bmr_on");
    }
    f.wave = f.sphere = f.side = f.page = -1;
    f.comb_dx = f.comb_sy = -1;
    f.doc = d;
    op1f_log(3, LOG_FILE, 1, "veffbode: faceplate %s", d ? "made" : "NOT made: the base's page stands in");
}

static void pick(void* const* nodes, int* now, int next) {
    if (next == *now) return;
    if (*now >= 0) show(nodes[*now], 0);
    if (next >= 0) show(nodes[next], 1);
    *now = next;
}

static void write_value(int c, const char* t) {
    for (int i = 0; i < PLACES_TEXT; ++i) {
        int g = -1;
        if (*t) { for (int k = 0; k < GLYPH_COUNT; ++k) if (GLYPHS[k] == *t) { g = k; break; } ++t; }
        const int was = f.shown_glyph[c][i];
        if (g == was) continue;
        if (was >= 0) show(f.glyph[c][i][was], 0);
        if (g >= 0) show(f.glyph[c][i][g], 1);
        f.shown_glyph[c][i] = (signed char)g;
    }
}

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

static int speed_of(int centihz) {
    int a = centihz < 0 ? -centihz : centihz, s = 1;
    while (a > 25 && s < 64) { a >>= 1; s += 4; }
    return s;
}

static u32 wrap(u32 acc, u32 room) { while (acc >= room) acc -= room; return acc; }

static void dress(u32 where, int shift) {
    int v[4];
    for (u32 i = 0; i < 4u; ++i) if (op1f_param_heard(&op1f_card_veffbode, ASK_EFFECT, where, i, &v[i]) != 0) v[i] = 0;
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
    const int hz = shift_centihz(v[V_SHIFT]);
    const u32 sp = (u32)speed_of(hz);
    f.wave_acc = wrap(f.wave_acc + sp, WAVE_FRAMES << 4);
    f.sphere_acc = wrap(f.sphere_acc + sp, SPHERE_FRAMES << 5);
    const int w = (int)(f.wave_acc >> 4), s = (int)(f.sphere_acc >> 5);
    pick(f.waves, &f.wave, hz < 0 ? WAVE_FRAMES - 1 - w : w);
    pick(f.spheres, &f.sphere, hz < 0 ? SPHERE_FRAMES - 1 - s : s);
    const int side = x[E_SIDEBAND] < 34 ? 1 : x[E_SIDEBAND] > 66 ? 0 : 2;   // 0 up, 1 down, 2 both
    const int up = hz < 0 ? side == 1 : side == 0, down = hz < 0 ? side == 0 : side == 1;
    pick(f.sides, &f.side, up ? 0 : down ? 1 : 2);
    for (int k = 0; k < 2; ++k) {
        const int lit = side == 2 || (k == 0 ? down : up);
        show(f.mark[k][0], !lit);
        show(f.mark[k][1], lit);
    }
    int a = hz < 0 ? -hz : hz, dx = 0;
    while (a > 12 && dx < 44) { a >>= 1; dx += 4; }
    const u32 now = level >> 20;
    f.lvl = now > f.lvl ? now : f.lvl - (f.lvl >> 3);
    int sy = 300 + (int)f.lvl * 3;
    if (sy > 1400) sy = 1400;
    sy &= ~31;
    if (dx != f.comb_dx || sy != f.comb_sy) {
        const int ty = 122 * (1024 - sy);
        const int ml[9] = { 1024, 0, 0, 0, sy, 0, -1024 * dx, ty, 1024 }, mr[9] = { 1024, 0, 0, 0, sy, 0, 1024 * dx, ty, 1024 };
        if (f.comb_l) op1f_move(f.comb_l, ml);
        if (f.comb_r) op1f_move(f.comb_r, mr);
        f.comb_dx = dx; f.comb_sy = sy;
    }
    const int pg = shift ? 1 : 0;
    if (pg != f.page) { for (int c = 0; c < 4; ++c) { show(f.labels[c][1 - pg], 0); show(f.labels[c][pg], 1); } f.page = pg; }
    for (int c = 0; c < 4; ++c) {
        char* q = f.line;
        int arc;
        if (pg == 0) {
            if (c == 0) { q = put_hundredths(q, hz); put(q, "hz"); arc = (held(v[0], -16383, 16383) + 16384) >> 11; }
            else if (c == 1) { put_number(q, held(v[1], 0, 359)); arc = (held(v[1], 0, 359) * 46) >> 10; }
            else { const int pc = held(v[c], 0, c == 2 ? 90 : 100); q = put_number(q, pc); put(q, "%"); arc = (pc * (c == 2 ? 182 : 164)) >> 10; }
        } else if (c == 0) {
            const int st = x[E_DELAY];
            if (st < NOTES) put(q, NOTE_NAMES[st]); else { q = put_number(q, step_ms(st - NOTES)); put(q, "ms"); }
            arc = (st * 176) >> 10;
        } else if (c == 1) {
            put(q, x[E_SIDEBAND] < 34 ? "down" : x[E_SIDEBAND] > 66 ? "up" : "both");
            arc = (x[E_SIDEBAND] * 164) >> 10;
        } else {
            const int pc = x[c == 2 ? E_TONE : E_SMEAR];
            q = put_number(q, pc); put(q, c == 3 && x[E_CROSS] ? "%x" : "%");
            arc = (pc * 164) >> 10;
        }
        write_value(c, f.line);
        arc = held(arc, 0, ARC_STEPS);
        if (arc != f.arcs[c]) {
            if (f.arcs[c] > 0) show(f.arc[c][f.arcs[c]], 0);
            if (arc > 0) show(f.arc[c][arc], 1);
            f.arcs[c] = arc;
        }
    }
}

extern "C" void veffbode_frame(const Op1fFrame* now) OP1F_NAMED("veffbode_frame");
extern "C" void veffbode_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    if (!f.asked) {
        f.asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
        make_page();
    }
    if (f.doc && !f.stood) f.stood = (u32)op1f_page_stand_in(&op1f_card_veffbode, ASK_EFFECT, f.doc);
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
    if (f.doc && now->page == &op1f_card_veffbode && (how & (u32)OP1F_SHOWN_UP)) dress(where, now->shift != 0);
}
