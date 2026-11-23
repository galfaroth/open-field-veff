// veffrrrr: a multi-stage distortion in the spirit of Ableton Live 12's Roar in its serial routing, as an effect of the
// unit's own lists (level 8 of open-field's base, level 7 for the values past the four knobs, level 10 for the page).
// Our own implementation from the behaviour. It changes no place of the firmware and names no address of it.
//
// What the owner meets:
//   `veffrrrr` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer), and on every
//   wagon of fx train. It stands on the unit's `nitro` (the unit's own drive-and-filter effect, the nearest in
//   character): a sound saved with it keeps nitro, switched off.
//   The four knobs: DRIVE (0..36 dB), S1 (stage 1 amount, 0..100), S2 (stage 2 amount), MIX (0..100).
//   With shift held the four knobs turn the values of a sub-page, kept with the sound (level 7, `veffrrrr_set`);
//   shift and a press of the 1st knob steps the sub-page: s1, s2, fb, out.
//     s1 / s2  shaper (sine, diode, fold, rect, crush, noise), bias (-50..50), filter (lp, bp, hp, comb), freq
//              (30 Hz..18 kHz); shift and a press of the 4th knob steps the filter's resonance (r0..r7)
//     fb       feedback amount (0..100), time (1/2..1/64 of the tempo, then 1..1000 ms), the loop's low-pass, tone
//              (the input's tilt, -50 dark .. +50 bright)
//     out      output level (-24..+12 dB)
//   A press of a knob (without shift) puts its value back to the start.
//
// Per side: drive -> tilt -> (+ feedback) -> [bias -> shaper -> filter] x2 -> dc blocker -> output -> mix.
// The feedback: the wet out -> a delay -> a low-pass -> a soft knee -> times amount times a gate that follows the
// dry input (about -36 dBFS opens it fully, it closes ~0.4 s after the input goes silent): the loop can sing while
// there is sound and can never latch once the sound is gone.
// On the audio thread: no division, no 64-bit multiply, no float; every sum held at full scale.

#include "build_r6e_card.h"
#include "veff_ui.h"
#include "veffrrrr_tables.h"

typedef unsigned int u32;
typedef int i32;
typedef short i16;

enum { ASK_EFFECT = 0, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { QUIET_LEVEL = 34000, HOLD_FRAMES = 44100 };                     // -96 dBFS; 1 s
enum { CHANNELS = 2, BLOCK = 64, STAGES = 2, COMB_ROOM = 1024, LINE = 46000, PLACES = 17 };
enum { V_DRIVE = 0, V_S1 = 1, V_S2 = 2, V_MIX = 3 };
enum { SH_SINE = 0, SH_DIODE, SH_FOLD, SH_RECT, SH_CRUSH, SH_NOISE, SHAPERS };
enum { F_LP = 0, F_BP, F_HP, F_COMB, FILTERS };
// the extras: per stage type, bias, filter, freq, res; then the feedback's amount, time, low-pass; tone; out
enum { E_T1 = 0, E_B1, E_F1, E_Q1, E_R1, E_T2, E_B2, E_F2, E_Q2, E_R2, E_FBA, E_FBT, E_FBL, E_TONE, E_OUT, EXTRAS };
enum { NOTES = 6, MS_STEPS = 76, DELAY_STEPS = NOTES + MS_STEPS, SUBS = 4 };
static const u32 BEATS_Q8[NOTES] = { 512, 256, 128, 64, 32, 16 };     // 1/2 .. 1/64 of a 4/4 bar, in beats * 256
static const char* const NOTE_NAMES[NOTES] = { "1/2", "1/4", "1/8", "1/16", "1/32", "1/64" };
static const int EXTRA_START[EXTRAS] = { SH_SINE, 0, F_LP, 56, 1, SH_DIODE, 10, F_LP, 46, 2, 0, NOTES + 22, 44, 0, 18 };
static const int EXTRA_LOW[EXTRAS] = { 0, -50, 0, 0, 0, 0, -50, 0, 0, 0, 0, 0, 0, -50, 0 };
static const int EXTRA_MOST[EXTRAS] = { SHAPERS - 1, 50, FILTERS - 1, 63, 7, SHAPERS - 1, 50, FILTERS - 1, 63, 7, 100, DELAY_STEPS - 1, 63, 50, 36 };
static const char NAMES[EXTRAS] = { 't', 'b', 'f', 'q', 'r', 'T', 'B', 'F', 'Q', 'R', 'k', 'd', 'l', 'n', 'o' };
static const int SUB_EXTRA[SUBS][4] = { { E_T1, E_B1, E_F1, E_Q1 }, { E_T2, E_B2, E_F2, E_Q2 }, { E_FBA, E_FBT, E_FBL, E_TONE }, { E_OUT, -1, -1, -1 } };
static const int STEP[EXTRAS] = { 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 2, 1 };
static const char* const SUB_NAMES[SUBS] = { "s1", "s2", "fb", "out" };
static const char* const SHAPER_NAMES[SHAPERS] = { "sine", "diode", "fold", "rect", "crush", "noise" };
static const char* const FILTER_NAMES[FILTERS] = { "lp", "bp", "hp", "comb" };
static const int KNOB_START[4] = { 12, 50, 30, 15 };
static const char KEY[] = "veffrrrr_set";
static const char LOG_FILE[] = "op1f/veffrrrr.cpp";

extern "C" const Op1fCard op1f_card_veffrrrr OP1F_NAMED("op1f_card_veffrrrr");

static int step_ms(int s) {                                            // 1..20, 25..100 by 5, 110..300 by 10, 325..1000 by 25
    if (s < 20) return s + 1;
    if (s < 35) return 20 + (s - 19) * 5;
    if (s < 55) return 100 + (s - 34) * 10;
    return 300 + (s - 54) * 25;
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
static inline i32 mulq(i32 a, i32 b) { return __builtin_bfin_mult_fr1x32x32(a, b); }
static inline i32 addq(i32 a, i32 b) { return __builtin_bfin_add_fr1x32(a, b); }
static inline i32 subq(i32 a, i32 b) { return __builtin_bfin_sub_fr1x32(a, b); }
#endif

static inline int held(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// the saturating left shift and magnitude: one instruction each on the unit (ASHIFT (S), ABS), the same values on the host
#ifdef OP1F_HOST
static inline i32 shl_sat(i32 v, int s) {
    const i32 top = 0x7fffffff >> s;
    return v > top ? 0x7fffffff : v < -top - 1 ? (i32)0x80000000 : (i32)((u32)v << s);
}
static inline i32 mag(i32 x) { return x < 0 ? (x == (i32)0x80000000 ? 0x7fffffff : 0 - x) : x; }
#else
static inline i32 shl_sat(i32 v, int s) { return __builtin_bfin_shl_fr1x32(v, (short)s); }
static inline i32 mag(i32 x) { return __builtin_bfin_abs_fr1x32(x); }
#endif
static inline i32 pct(int v) { return v >= 100 ? 0x7fffffff : v * 21474836; }   // 0..100 to Q31

// ---------------------------------------------------------------- the values past the four knobs
static volatile int extra[PLACES][EXTRAS];
static volatile u32 extra_ready;
static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- the sound (audio thread)
struct Stage { i32 ic1, ic2, dc; u32 cpos; };
struct Side { Stage s[STAGES]; i32 tilt, dc, fblp, env, wg, wg_to; u32 quiet; };
struct State {
    Side ch[CHANNELS];
    u32 wpos, delay_q8, filled, noise;
    i32 mix, drive;
    i32 comb[CHANNELS][STAGES][COMB_ROOM];
    i16 line[CHANNELS][LINE];
};
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 200544, "the recipe's state_bytes");

static inline i32 sine(u32 phase) {
    const u32 i = phase >> 22;
    const i32 f = (i32)((phase >> 6) & 0xffffu) << 15;
    const i32 s0 = SINE[i], s1 = SINE[i + 1];
    return s0 + mulq((s1 >> 1) - (s0 >> 1), f) * 2;
}

static inline i32 soft(i32 x) { return subq(x, mulq(mulq(x, x), x) >> 2); }       // x - x^3/4
static inline i32 cubic(i32 w) { return addq(w, (w >> 1) - (mulq(mulq(w, w), w) >> 1)); }   // 1.5w - 0.5w^3

struct StageSet { int type, ftype; i32 bias, g, amt; i32 a1, a2, a3, khalf, cfb, cin; u32 clen; };

static void stage_set(StageSet* s, const int* ex, int amount) {
    s->type = held(ex[0], 0, SHAPERS - 1);
    s->bias = held(ex[1], -50, 50) * 21474836;
    s->ftype = held(ex[2], 0, FILTERS - 1);
    const int fq = held(ex[3], 0, 63), r = held(ex[4], 0, 7);
    const int a = held(amount, 0, 100);
    s->g = AMT_G[a];
    s->amt = pct(a);
    const int* c = &SVF[(fq * 8 + r) * 3];
    s->a1 = c[0]; s->a2 = c[1]; s->a3 = c[2]; s->khalf = K_HALF[r];
    s->cfb = COMB_FB[r]; s->cin = 0x7fffffff - COMB_FB[r]; s->clen = (u32)COMB_LEN[fq];
}

static void lcg_jump(u32 j, u32* a, u32* c) {                         // the noise's step taken j times: x*a + c
    u32 ma = 1u, mc = 0u;
    for (u32 i = 0; i < j; ++i) { ma *= 1664525u; mc = mc * 1664525u + 1013904223u; }
    *a = ma; *c = mc;
}

// the shaper; `nv` the noise draw (the noise rides the signal, not the bias)
template <int TYPE>
static inline i32 shape_t(i32 bias, i32 g, i32 amt, i32 x, u32 nv) {
    const i32 in = x;
    x = addq(x, bias);
    switch (TYPE) {
    case SH_SINE: {                                                    // sin(pi/2 * clamp(x * gain))
        i32 u = mulq(x, g);
        u = u > 0x08000000 ? 0x08000000 : u < -0x08000000 ? -0x08000000 : u;
        return sine((u32)u << 3);
    }
    case SH_FOLD: return sine((u32)mulq(x, g) << 3);                   // the same sine, unbounded: a folder
    case SH_DIODE: {                                                   // soft on the up side, harder and lower below
        const i32 v = shl_sat(mulq(x, g), 4);
        if (v >= 0) return cubic(v);
        return mulq(cubic(shl_sat(v, 1)), 0x33333333);
    }
    case SH_RECT: {                                                    // x toward |x|, by the amount
        const i32 d = (mag(x) >> 1) - (x >> 1);
        return addq(x, shl_sat(mulq(amt, d), 1));
    }
    case SH_CRUSH: {                                                   // 16 bits down to 2, toward 0
        const int drop = 16 + ((held((amt >> 16) * 14, 0, 14 * 32768) * 2) >> 16);
        const u32 sign = 0u - ((u32)x >> 31);
        const u32 m = (((u32)x ^ sign) - sign) & (0xffffffffu << drop);
        return (i32)((m ^ sign) - sign);
    }
    default: return addq(x, mulq(mulq((i32)nv, amt), in));             // noise riding the signal: x(1 + a n)
    }
}

// one stage (shaper then filter) over a chunk, in place; `lane` is this stage's first noise draw, (fa, fc) a frame's step.
// Templated on the shaper and the filter so the loop carries no per-frame branch. `prime` seeds the dc follower with
// the first shaped frame before the loop (the same as setting it inside: the first step then moves it by zero).
template <int TYPE, int FT>
__attribute__((noinline)) static void stage_pass(const StageSet* s, Stage* st, i32* comb, i32* xb, u32 len, u32 lane, u32 fa, u32 fc, int prime) {
    i32 dc = st->dc;
    const i32 bias = s->bias, g = s->g, amt = s->amt;
    if (prime) dc = shape_t<TYPE>(bias, g, amt, xb[0], lane);
    if (FT == F_COMB) {
        u32 cpos = st->cpos;
        const u32 clen = s->clen; const i32 cin = s->cin, cfb = s->cfb;
        for (u32 i = 0; i < len; ++i) {
            i32 x = shape_t<TYPE>(bias, g, amt, xb[i], lane);
            if (TYPE == SH_NOISE) lane = lane * fa + fc;
            dc = addq(dc, mulq(subq(x, dc), 3060000));
            x = subq(x, dc);
            const i32 back = comb[(cpos - clen) & (COMB_ROOM - 1)];
            const i32 y = addq(mulq(x, cin), mulq(back, cfb));
            comb[cpos] = y;
            cpos = (cpos + 1) & (COMB_ROOM - 1);
            xb[i] = shl_sat(y, 1);
        }
        st->cpos = cpos;
    } else {
        i32 ic1 = st->ic1, ic2 = st->ic2;
        const i32 a1 = s->a1, a2 = s->a2, a3 = s->a3, khalf = s->khalf;
        for (u32 i = 0; i < len; ++i) {
            i32 x = shape_t<TYPE>(bias, g, amt, xb[i], lane);
            if (TYPE == SH_NOISE) lane = lane * fa + fc;
            dc = addq(dc, mulq(subq(x, dc), 3060000));
            x = subq(x, dc);
            const i32 v0 = x >> 4;
            const i32 v3 = subq(v0, ic2);
            const i32 v1 = addq(mulq(a1, ic1), mulq(a2, v3));
            const i32 v2 = addq(ic2, addq(mulq(a2, ic1), mulq(a3, v3)));
            ic1 = subq(addq(v1, v1), ic1);
            ic2 = subq(addq(v2, v2), ic2);
            i32 y;
            if (FT == F_LP) y = v2;
            else if (FT == F_BP) y = v1;
            else { const i32 kv = mulq(khalf, v1); y = subq(subq(v0, addq(kv, kv)), v2); }
            xb[i] = shl_sat(y, 4);
        }
        st->ic1 = ic1; st->ic2 = ic2;
    }
    st->dc = dc;
}

template <int TYPE>
static void stage_run(const StageSet* s, Stage* st, i32* comb, i32* xb, u32 len, u32 lane, u32 fa, u32 fc, int prime) {
    switch (s->ftype) {
    case F_LP: stage_pass<TYPE, F_LP>(s, st, comb, xb, len, lane, fa, fc, prime); break;
    case F_BP: stage_pass<TYPE, F_BP>(s, st, comb, xb, len, lane, fa, fc, prime); break;
    case F_HP: stage_pass<TYPE, F_HP>(s, st, comb, xb, len, lane, fa, fc, prime); break;
    default: stage_pass<TYPE, F_COMB>(s, st, comb, xb, len, lane, fa, fc, prime); break;
    }
}

// the input over a chunk: the gate's envelope, drive, then (each only when on) tone and the feedback's return.
// Tone 0 skips the tone's term (exactly zero then); feedback 0 skips the
// loop's return term (the loop's low-pass still follows the line, so turning it up later is exact).
template <bool FB, bool TONE>
__attribute__((noinline)) static void front_pass(Side* ch, const int* io, const i16* line, i32* xb, u32 len, u32 d, u32 target, u32 w, u32 filled,
                       i32 dg, i32 dstep, i32 tilt_c, i32 tone, i32 fb, i32 fbl) {
    i32 env = ch->env, fblp = ch->fblp, tilt = ch->tilt;
    for (u32 i = 0; i < len; ++i) {
        dg += dstep;
        const i32 dry = io[i];
        const i32 a = mag(dry);
        env = a > env ? a : env - (env >> 12) - (env > 0);
        i32 x = shl_sat(mulq(dry, dg), 6);
        tilt = addq(tilt, mulq(subq(x, tilt), tilt_c));
        if (TONE) x = addq(x, mulq(tone, subq(subq(x, tilt), tilt)));
        d += (u32)(((i32)target - (i32)d) >> 9);
        const u32 whole = d >> 8, frac = d & 255u;
        i32 back = 0;
        if (whole + 1u <= filled) {
            int r0 = (int)w - (int)whole; if (r0 < 0) r0 += LINE;
            int r1 = r0 - 1; if (r1 < 0) r1 += LINE;
            const i32 a0 = line[r0], a1 = line[r1];
            back = (a0 * 256 + (a1 - a0) * (i32)frac) * 256;
        }
        fblp = addq(fblp, mulq(subq(back, fblp), fbl));
        if (FB) x = addq(x, mulq(mulq(fb, shl_sat(env, 6)), soft(fblp)));
        if (++w >= (u32)LINE) w = 0;
        if (filled < (u32)LINE) ++filled;
        xb[i] = x;
    }
    ch->env = env; ch->fblp = fblp; ch->tilt = tilt;
}

// the output over a chunk: dc, level, the wet fade, into the feedback line and the mix (FULL: mix held at 100,
// where the dry's weight is exactly zero)
template <bool FULL>
__attribute__((noinline)) static void tail_pass(Side* ch, int* io, i16* line, const i32* xb, u32 len, u32 w, i32 m, i32 step, i32 outg) {
    i32 dc = ch->dc, wg = ch->wg;
    const i32 wg_to = ch->wg_to;
    for (u32 i = 0; i < len; ++i) {
        const i32 x = xb[i];
        dc = addq(dc, mulq(subq(x, dc), 3060000));
        wg = wg_to ? wg + ((wg_to - wg) >> 6) : wg - (wg >> 9) - (wg > 0);
        const i32 wet = mulq(shl_sat(mulq(subq(x, dc), outg), 2), wg);
        line[w] = (i16)(wet >> 16);
        if (FULL) io[i] = mulq(0x7fffffff, wet);
        else { m += step; io[i] = addq(mulq(0x7fffffff - m, io[i]), mulq(m, wet)); }
        if (++w >= (u32)LINE) w = 0;
    }
    ch->dc = dc; ch->wg = wg;
}

static u32 delay_target_q8(int step, u32 period) {
    const int s = held(step, 0, DELAY_STEPS - 1);
    u32 q;
    if (s < NOTES) {
        u32 p = period >> 6;
        if (p > 40000000u) p = 40000000u;
        const u32 beat = (p * 96u) >> 12;
        q = beat * BEATS_Q8[s];
    } else q = (u32)step_ms(s - NOTES) * 11290u;
    const u32 most = (u32)(LINE - 4) << 8;
    return q > most ? most : q;
}

static void clear_small(State& st) {
    u32* w = (u32*)st.ch;
    for (u32 i = 0; i < sizeof(st.ch) / 4; ++i) w[i] = 0;
    u32* c = (u32*)st.comb;
    for (u32 i = 0; i < sizeof(st.comb) / 4; ++i) c[i] = 0;
    st.filled = 0;
    st.mix = 0;
    st.drive = DRIVE_Q[0];
}

// ---------------------------------------------------------------- the page's audio feed (written by the audio thread)
// Plain 32-bit stores after the block is done, only for the place on the page; the frame reads them, torn reads are
// harmless. Nothing here is read back by the sound.
enum { VIS_WAVE = 48, VIS_STEP = 16 };
static volatile u32 shown_where, shown_how;
static volatile i32 vis_wave[VIS_WAVE];                                // the left output, one frame in 16
static volatile u32 vis_pos, vis_tick;
static volatile i32 vis_env, vis_gate;                                 // the dry input's envelope, the wet gate

extern "C" void veffrrrr_process(const Op1fEffectBlock* now) OP1F_NAMED("veffrrrr_process");
extern "C" void veffrrrr_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    if (now->fresh) clear_small(st);
    const int* v = now->values;
    const volatile int* ev = extras_of(now->place);
    int ex[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) ex[k] = ev ? ev[k] : EXTRA_START[k];
    const i32 want = pct(held(v[V_MIX], 0, 100));
    const i32 from = st.mix;
    if (!now->on || (want == 0 && from == 0)) return;                  // off, or mix 0: the sound as it came
    StageSet s[STAGES];
    stage_set(&s[0], &ex[E_T1], v[V_S1]);
    stage_set(&s[1], &ex[E_T2], v[V_S2]);
    const i32 drive_to = DRIVE_Q[held(v[V_DRIVE], 0, 36)], drive_from = st.drive;
    const i32 dstep = (drive_to >> 6) - (drive_from >> 6);
    const i32 tilt_c = ONEPOLE[32];                                    // the tilt's hinge, ~770 Hz
    const i32 tone = held(ex[E_TONE], -50, 50) * 42949672;             // -1 .. +1
    const i32 fb = mulq(pct(held(ex[E_FBA], 0, 100)), 0x7999999a);     // 0 .. 0.95
    const i32 fbl = ONEPOLE[held(ex[E_FBL], 0, 63)];
    const i32 outg = OUT_Q[held(ex[E_OUT], 0, 36)];
    const u32 target = delay_target_q8(ex[E_FBT], now->period);
    const i32 step = (want >> 6) - (from >> 6);
    int* bus[CHANNELS] = { now->left, now->right };
    const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    // the wet gate, once a block a side: open while the input sounds, held 1 s after it falls below -96 dBFS, then
    // faded out; fully closed, the side's small memories are cleared, so nothing a quiet input left can ring on
    for (int c = 0; c < CHANNELS; ++c) {
        Side* ch = &st.ch[c];
        if (ch->env > QUIET_LEVEL) ch->quiet = 0;
        else if (ch->quiet < (u32)HOLD_FRAMES + 4096u) ch->quiet += frames;
        ch->wg_to = ch->quiet >= (u32)HOLD_FRAMES ? 0 : 0x7fffffff;
        if (ch->quiet >= (u32)HOLD_FRAMES + 4096u && ch->wg == 0 && ch->tilt | ch->dc | ch->fblp | ch->s[0].ic1 | ch->s[0].ic2 | ch->s[1].ic1 | ch->s[1].ic2 | ch->s[0].dc | ch->s[1].dc) {
            ch->tilt = ch->dc = ch->fblp = 0;
            for (int k = 0; k < STAGES; ++k) {
                ch->s[k].ic1 = ch->s[k].ic2 = ch->s[k].dc = 0;
                for (u32 i = 0; i < (u32)COMB_ROOM; ++i) st.comb[c][k][i] = 0;
            }
        }
    }
    // block processing: each pass (the input, each stage, the output) runs over a chunk of frames with its memory in
    // registers. Exact: the chunk is cut so that no feedback read in it falls on a frame written in it.
    const int nn = (s[0].type == SH_NOISE) + (s[1].type == SH_NOISE);   // noise draws per side per frame
    u32 fa, fc; lcg_jump((u32)(2 * nn), &fa, &fc);                         // one frame's draws, both sides
    i32 m = from, dg = drive_from;
    u32 noise = st.noise;
    i32 xb[BLOCK];
    const int full = want == 0x7fffffff && from == 0x7fffffff;
    for (u32 n0 = 0; n0 < frames;) {
        const u32 d0 = st.delay_q8, wpos0 = st.wpos, filled0 = st.filled;
        u32 len = 0;
        for (u32 d = d0; n0 + len < frames; ++len) {
            d += (u32)(((i32)target - (i32)d) >> 9);
            if (len && (d >> 8) < len + 1u) break;
        }
        const int prime = now->fresh && n0 == 0;
        for (int c = 0; c < CHANNELS; ++c) {
            Side* ch = &st.ch[c];
            int* io = bus[c] + n0;
            if (fb) {
                if (tone) front_pass<true, true>(ch, io, st.line[c], xb, len, d0, target, wpos0, filled0, dg, dstep, tilt_c, tone, fb, fbl);
                else front_pass<true, false>(ch, io, st.line[c], xb, len, d0, target, wpos0, filled0, dg, dstep, tilt_c, tone, fb, fbl);
            } else {
                if (tone) front_pass<false, true>(ch, io, st.line[c], xb, len, d0, target, wpos0, filled0, dg, dstep, tilt_c, tone, fb, fbl);
                else front_pass<false, false>(ch, io, st.line[c], xb, len, d0, target, wpos0, filled0, dg, dstep, tilt_c, tone, fb, fbl);
            }
            for (int k = 0; k < STAGES; ++k) {
                const int o = c * nn + (k == 1 && s[0].type == SH_NOISE);
                u32 ja, jc; lcg_jump((u32)o + 1u, &ja, &jc);
                const u32 lane = noise * ja + jc;
                i32* cb = st.comb[c][k];
                switch (s[k].type) {
                case SH_SINE: stage_run<SH_SINE>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                case SH_DIODE: stage_run<SH_DIODE>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                case SH_FOLD: stage_run<SH_FOLD>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                case SH_RECT: stage_run<SH_RECT>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                case SH_CRUSH: stage_run<SH_CRUSH>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                default: stage_run<SH_NOISE>(&s[k], &ch->s[k], cb, xb, len, lane, fa, fc, prime); break;
                }
            }
            if (full) tail_pass<true>(ch, io, st.line[c], xb, len, wpos0, m, step, outg);
            else tail_pass<false>(ch, io, st.line[c], xb, len, wpos0, m, step, outg);
        }
        u32 d = d0;
        for (u32 i = 0; i < len; ++i) {
            d += (u32)(((i32)target - (i32)d) >> 9);
            noise = noise * fa + fc;
        }
        st.delay_q8 = d;
        u32 w = wpos0 + len; if (w >= (u32)LINE) w -= LINE;
        st.wpos = w;
        st.filled = filled0 + len > (u32)LINE ? (u32)LINE : filled0 + len;
        m += step * (i32)len; dg += dstep * (i32)len;
        n0 += len;
    }
    st.noise = noise;
    st.mix = want;
    st.drive = drive_to;
    if (now->place == shown_where) {                                   // the page's feed: 4 words of wave, 3 more
        const int* l = now->left;
        u32 q = vis_pos;
        for (u32 i = 0; i < frames; i += (u32)VIS_STEP) {
            vis_wave[q] = l[i];
            if (++q >= (u32)VIS_WAVE) q = 0;
        }
        vis_pos = q;
        vis_env = st.ch[0].env > st.ch[1].env ? st.ch[0].env : st.ch[1].env;
        vis_gate = st.ch[0].wg;
        vis_tick = vis_tick + 1u;
    }
}

extern "C" void veffrrrr_reset(unsigned place, void* state) OP1F_NAMED("veffrrrr_reset");
extern "C" void veffrrrr_reset(unsigned place, void* state) {
    (void)place;
    if (state == 0) return;
    u32* w = (u32*)state;
    for (u32 i = 0; i < (u32)(STATE_BYTES >> 2); ++i) w[i] = 0;
}

// ---------------------------------------------------------------- told by the base (display thread)
static volatile u32 reread;

extern "C" void veffrrrr_shown(unsigned where, unsigned how) OP1F_NAMED("veffrrrr_shown");
extern "C" void veffrrrr_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffrrrr_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffrrrr_changed");
extern "C" void veffrrrr_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread)
static volatile int pending_turn[4];
static volatile u32 pending_click;                                     // bits 0..3 knob presses; 4 next sub-page; 5 resonance

extern "C" int veffrrrr_input(const Op1fInput* e) OP1F_NAMED("veffrrrr_input");
extern "C" int veffrrrr_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {
        const int steps = e->turn >> 10;
        if (steps) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        if (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) { if (knob == 0) pending_click |= 16u; else if (knob == 3) pending_click |= 32u; }
        else pending_click |= 1u << knob;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)PRESS_FIRST && e->key <= (u32)PRESS_LAST) return 1;
    return 0;
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
static char* put_signed(char* p, int v) { if (v > 0) *p++ = '+'; return put_number(p, v); }

static void extras_start(u32 p) { for (int k = 0; k < EXTRAS; ++k) extra[p][k] = EXTRA_START[k]; }

static int read_member(const char* t, char name, int* out) {
    for (; *t; ++t) {
        if (t[0] == '"' && t[1] == name && t[2] == '"') {
            const char* q = t + 3;
            while (*q == ' ' || *q == ':') ++q;
            int sign = 1, v = 0, any = 0;
            if (*q == '-') { sign = -1; ++q; }
            while (*q >= '0' && *q <= '9' && v < 100000) { v = v * 10 + (*q - '0'); ++q; any = 1; }
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
    char text[160];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffrrrr, where, which, KEY, text, sizeof text) > 0)
        for (int k = 0; k < EXTRAS; ++k) { int x; if (read_member(text, NAMES[k], &x)) extra[p][k] = held(x, EXTRA_LOW[k], EXTRA_MOST[k]); }
    extra_ready |= 1u << p;
}

static u32 unsaved;

static void extras_write(u32 p) {                                      // {"t":0,"b":0,...,"v":1}: about 90 bytes
    unsigned where, which;
    where_of(p, &where, &which);
    char text[160], * q = text;
    *q++ = '{';
    for (int k = 0; k < EXTRAS; ++k) { *q++ = '"'; *q++ = NAMES[k]; q = put(q, "\":"); q = put_number(q, extra[p][k]); *q++ = ','; }
    put(q, "\"v\":1}");
    if (op1f_value_set(&op1f_card_veffrrrr, where, which, KEY, text) == 0) unsaved &= ~(1u << p);
}

// the page: the common Veff page (veff_ui.h) on the base's blank page, and the RRRR picture in its band
static const char* const KNOB_LABELS[4] = { "DRIVE", "S1", "S2", "MIX" };
static const char* const SUB_LABELS[SUBS][4] = { { "SHAPE", "BIAS", "FILTER", "FREQ" }, { "SHAPE", "BIAS", "FILTER", "FREQ" },
                                                 { "AMOUNT", "TIME", "LP", "TONE" }, { "OUT", "", "", "" } };
static const char* const SUB_TAGS[SUBS] = { "S1", "S2", "FB", "OUT" };
static int sub;                                                        // the shift page's sub-page
// (1024 << 8) / range, folded at compile time: a value's slider is (v - low) * this >> 8
#define VEFF_R(n) ((1024 << 8) / (n))
static const int EXTRA_RECIP[EXTRAS] = { VEFF_R(SHAPERS - 1), VEFF_R(100), VEFF_R(FILTERS - 1), VEFF_R(63), VEFF_R(7),
                                         VEFF_R(SHAPERS - 1), VEFF_R(100), VEFF_R(FILTERS - 1), VEFF_R(63), VEFF_R(7),
                                         VEFF_R(100), VEFF_R(DELAY_STEPS - 1), VEFF_R(63), VEFF_R(100), VEFF_R(36) };
static const int KNOB_RECIP[4] = { VEFF_R(36), VEFF_R(100), VEFF_R(100), VEFF_R(100) };

static char* put_hz_long(char* p, int hz) {                            // 850 Hz, 1.6 kHz, 12 kHz
    if (hz < 1000) { p = put_number(p, hz); return put(p, " Hz"); }
    u32 n = (u32)hz, k = 0;
    while (n >= 1000u) { n -= 1000u; ++k; }
    p = put_number(p, (int)k);
    if (k < 10u) { u32 t = 0; while (n >= 100u) { n -= 100u; ++t; } p = put(p, "."); p = put_number(p, (int)t); }
    return put(p, " kHz");
}

static void extra_text(char* q, int k, const int* x) {
    const int v = x[k];
    switch (k) {
    case E_T1: case E_T2: put(q, SHAPER_NAMES[held(v, 0, SHAPERS - 1)]); break;
    case E_F1: case E_F2: put(q, FILTER_NAMES[held(v, 0, FILTERS - 1)]); break;
    case E_Q1: case E_Q2: {
        const int r = x[k + 1];
        (void)r; put_hz_long(q, FREQ_HZ[held(v, 0, 63)]);
        break;
    }
    case E_FBL: put_hz_long(q, FREQ_HZ[held(v, 0, 63)]); break;
    case E_FBT: if (v < NOTES) put(q, NOTE_NAMES[held(v, 0, NOTES - 1)]); else { q = put_number(q, step_ms(v - NOTES)); put(q, " ms"); } break;
    case E_FBA: q = put_number(q, v); put(q, "%"); break;
    case E_OUT: q = put_signed(q, v - 24); put(q, " dB"); break;
    default: put_signed(q, v); break;
    }
}

// The picture, after the reference sheet's RRRR panel: a row of seven circles along the axis, growing towards the
// middle (s1's stage on the left, s2's on the right, both in the middle), dim pink fills (brighter where two overlap),
// pink outlines, white stems and white ring dots at their centres; a dashed pink ring around the middle one for the
// feedback loop. Sizes follow drive, s1, s2 and the envelope; each circle's grain follows its stage's shaper, filter
// and frequency and the output's wave; out scales the whole. Integer maths only.
static const int OUT_Q8[37] = { 102, 106, 110, 114, 119, 123, 128, 133, 139, 144, 150, 155, 162, 168, 174, 181, 188, 196,
                                203, 211, 220, 228, 237, 246, 256, 266, 276, 287, 298, 310, 322, 335, 348, 362, 376, 390, 406 };
enum { CIRCLES = 7, R_MOST = veffui::VEFF_ART_HALF };                  // the shared peak, 50 px
static const int CIRCLE_OFF[CIRCLES] = { -111, -82, -44, 0, 44, 82, 111 };
static const int CIRCLE_BASE[CIRCLES] = { 10, 20, 33, 47, 33, 20, 10 };
static u32 frame_count, seen_tick;
static int fenv;                                                       // the smoothed envelope, Q8

static inline int sin8(u32 phase) { return SINE[phase >> 22] >> 23; }  // -256 .. 256

static void paint_picture(void* c, u32 where, int shift, int on) {
    using namespace veffui;
    const u32 frame = ++frame_count;
    int x[EXTRAS], kv[4];
    for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
    for (u32 i = 0; i < 4u; ++i) { int val = KNOB_START[i]; if (op1f_param_heard(&op1f_card_veffrrrr, ASK_EFFECT, where, i, &val) != 0) val = KNOB_START[i]; kv[i] = val; }
    const u32 tick = vis_tick;
    const int live = tick != seen_tick && on;
    seen_tick = tick;
    int target = live ? (vis_env >> 22) : 0;                           // Q31 peak to Q8 (x 2: a quiet peak reads larger)
    target = target > 256 ? 256 : target;
    fenv += ((target - fenv) * 61) >> 8;
    const int drive = held(kv[V_DRIVE], 0, 36);
    const int outg = OUT_Q8[held(x[E_OUT], 0, 36)];
    const int ay = VEFF_AXIS_Y;
    axis(c);
    int cx[CIRCLES], cy[CIRCLES], r[CIRCLES];
    const u32 q0 = vis_pos;
    for (int j = 0; j < CIRCLES; ++j) {
        const int side = j < 3 ? 0 : j > 3 ? 1 : 2;                    // s1, s2, both
        const int b = side == 1 ? E_T2 : E_T1;
        const int v = side == 2 ? (held(kv[V_S1], 0, 100) + held(kv[V_S2], 0, 100)) >> 1 : held(kv[side ? V_S2 : V_S1], 0, 100);
        const int kind = held(x[b], 0, SHAPERS - 1), filter = held(x[b + 2], 0, FILTERS - 1);
        const int hz = FREQ_HZ[held(x[b + 3], 0, 63)], bias = held(x[b + 1], -50, 50);
        const u32 rate = (u32)((2000 + ((hz * 5) >> 4)) * 5833) >> 8;
        const u32 ph = (u32)j * (u32)((1200 + 300 * kind) * 683565) + frame * rate + (u32)filter * 683565276u;
        const int grain = sin8(ph);                                    // -256..256
        int g = 120 + drive * 3 + ((v * 3) >> 2) + ((fenv * 3) >> 2);
        g += (grain * (16 + v)) >> 9;
        if (live) { u32 q = q0 + (u32)j * 6u; if (q >= (u32)VIS_WAVE) q -= (u32)VIS_WAVE; g += (vis_wave[q] >> 24) >> 1; }
        g = (g * outg) >> 8;
        int rr = (CIRCLE_BASE[j] * g) >> 8;
        const int most = R_MOST - (j == 3 && held(x[E_FBA], 0, 100) ? 6 : 0);   // room for the loop's ring
        rr = rr < 3 ? 3 : rr > most ? most : rr;
        if (j != 3) { int lim = CIRCLE_BASE[j] + (CIRCLE_BASE[j] >> 1) + 4; lim = lim > R_MOST - 4 ? R_MOST - 4 : lim; rr = rr > lim ? lim : rr; }
        r[j] = rr;
        cx[j] = VEFF_MID_X + CIRCLE_OFF[j];
        cy[j] = ay + (side == 2 ? 0 : (bias * 5) >> 6);
    }
    const int glow = on ? 64 + (fenv >> 2) : 40;
    const unsigned fill = pink(glow), lens = pink(glow * 2 + 20);
    for (int j = 0; j < CIRCLES; ++j) {                                // the fills, two rows a box, and the lenses
        const int rr = r[j];
        for (int dy = -rr; dy <= rr; dy += 3) {
            const int w = isqrt(rr * rr - dy * dy);
            op1f_paint_box(c, cx[j] - w, cy[j] + dy, 2 * w + 1, 3, fill);
        }
    }
    for (int j = 0; j + 1 < CIRCLES; ++j) {
        const int ra = r[j], rb = r[j + 1];
        if (cx[j] + ra <= cx[j + 1] - rb) continue;
        for (int dy = -ra; dy <= ra; dy += 3) {
            const int yb = cy[j] + dy - cy[j + 1];
            if (yb < -rb || yb > rb) continue;
            const int wa = isqrt(ra * ra - dy * dy), wb = isqrt(rb * rb - yb * yb);
            const int lo = cx[j + 1] - wb, hi = cx[j] + wa;
            if (hi > lo) op1f_paint_box(c, lo, cy[j] + dy, hi - lo + 1, 3, lens);
        }
    }
    const unsigned edge = on ? VEFF_PINK : VEFF_GREY;
    for (int j = 0; j < CIRCLES; ++j) {
        const int big = shift && ((sub == 0 && j < 3) || (sub == 1 && j > 3));
        ring(c, cx[j], cy[j], r[j], r[j], edge, 1 + big, j == 3 ? 5 : 4);
        if (j == 3) op1f_paint_line(c, cx[j], cy[j] - r[j], cx[j], cy[j] + r[j], 1, VEFF_WHITE);
        else op1f_paint_line(c, cx[j], cy[j], cx[j], cy[j] + r[j], 1, VEFF_WHITE);
        op1f_paint_box(c, cx[j] - 2, cy[j] - 2, 5, 5, 0);
        ring(c, cx[j], cy[j], 3, 3, VEFF_WHITE, 1, 2);
    }
    const int amount = held(x[E_FBA], 0, 100);                         // the feedback loop: a turning dashed ring
    if (amount) {
        int bright = 64 + ((amount * (live ? 96 + (((vis_gate >> 24) * 5) >> 2) : 96)) >> 7);
        bright = bright > 256 ? 256 : bright;
        int rr = r[3] + 5 + (amount >> 4);
        rr = rr > R_MOST ? R_MOST : rr;
        const int speed = 1 + ((FREQ_HZ[held(x[E_FBL], 0, 63)] * 3) >> 14) + ((held(x[E_TONE], -50, 50) + 50) >> 4);
        const int spin = (int)(frame * (u32)speed);
        const int dashes = 6 + (held(x[E_FBT], 0, DELAY_STEPS - 1) >> 3);   // longer time, more dashes
        const unsigned colour = pink(bright);
        const int n = dashes > 16 ? 16 : dashes;
        const int stepa = 256 >> 4;                                     // 16 slots a turn, the first n of them lit
        for (int i = 0; i < 16; ++i) {
            if (i >= n) break;
            const int a = spin + i * stepa;
            op1f_paint_line(c, cx[3] + ((rr * cos256(a)) >> 14), cy[3] + ((rr * sin256(a)) >> 14),
                            cx[3] + ((rr * cos256(a + 8)) >> 14), cy[3] + ((rr * sin256(a + 8)) >> 14), 1, colour);
        }
    }
    sparkles(c);
}

static void paint(void* canvas, u32 where, u32 how, int shift) {
    using namespace veffui;
    const int on = !(how & (u32)OP1F_SHOWN_OFF);
    paint_picture(canvas, where, shift, on);
    char t[4][24], rlabel[12];
    Column col[4];
    if (!shift) {
        for (u32 i = 0; i < 4u; ++i) {
            int val = KNOB_START[i];
            if (op1f_param_heard(&op1f_card_veffrrrr, ASK_EFFECT, where, i, &val) != 0) val = KNOB_START[i];
            if (i == 0) { char* q = put_signed(t[i], val); put(q, " dB"); }
            else { char* q = put_number(t[i], val); put(q, "%"); }
            col[i].label = KNOB_LABELS[i]; col[i].value = t[i];
            col[i].frac = (held(val, 0, i == 0 ? 36 : 100) * KNOB_RECIP[i]) >> 8;
        }
    } else {
        int x[EXTRAS];
        for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
        for (int i = 0; i < 4; ++i) {
            const int k = SUB_EXTRA[sub][i];
            col[i].label = k < 0 ? "" : SUB_LABELS[sub][i]; col[i].value = t[i]; t[i][0] = 0; col[i].frac = 0;
            if (k < 0) continue;
            extra_text(t[i], k, x);
            col[i].frac = ((x[k] - EXTRA_LOW[k]) * EXTRA_RECIP[k]) >> 8;
            if ((k == E_Q1 || k == E_Q2) && x[k + 1]) { char* q = put(rlabel, "FREQ R"); put_number(q, x[k + 1]); col[i].label = rlabel; }
        }
    }
    page(canvas, "RRRR", ICON_RRRR, col, shift ? SUB_TAGS[sub] : 0, on);
}


static u32 asked;

extern "C" void veffrrrr_frame(const Op1fFrame* now) OP1F_NAMED("veffrrrr_frame");
extern "C" void veffrrrr_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffrrrr, "veffrrrrpage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffrrrr, ASK_EFFECT, page_doc);
    }
    if (!asked) {
        asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
        op1f_log(3, LOG_FILE, 1, "veffrrrr: ready, animated page");
    }
    const u32 riding = (u32)op1f_riding(&op1f_card_veffrrrr, ASK_EFFECT);
    const u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        for (int k = 0; k < 4; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            const int e = SUB_EXTRA[sub][k];
            if (e < 0) continue;
            extra[where][e] = held(extra[where][e] + t * STEP[e], EXTRA_LOW[e], EXTRA_MOST[e]);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 16u) sub = sub + 1 >= SUBS ? 0 : sub + 1;
        if ((clicks & 32u) && sub < 2) {
            const int e = sub == 0 ? E_R1 : E_R2;
            extra[where][e] = extra[where][e] >= 7 ? 0 : extra[where][e] + 1;
            unsaved |= 1u << where;
        }
        for (u32 k = 0; k < 4u; ++k) if (clicks >> k & 1u) op1f_param_set(&op1f_card_veffrrrr, ASK_EFFECT, where, k, KNOB_START[k]);
    } else {
        for (int k = 0; k < 4; ++k) pending_turn[k] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffrrrr && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP)) paint(now->canvas, where, how, now->shift != 0);
}
