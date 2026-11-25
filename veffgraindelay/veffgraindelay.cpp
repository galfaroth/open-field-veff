// veffgraindelay: a grain delay, as an effect of the unit's own lists (level 8 of open-field's base, with level 7 for
// the values past the four knobs and level 10 for the page). It changes no place of the firmware and names no address
// of it.
//
// What the owner meets:
//   `veffgraindelay` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer), and on
//   every wagon of fx train. It stands on the unit's `delay`: a sound saved with it keeps delay, switched off.
//   The four knobs (and the LFOs): PITCH (-24 .. +24 semitones, the knob's distance from the middle squared: fine near
//   0), SPRAY (0 .. 500 ms of random spread of each grain's read position), FEEDBACK (0 .. 95), MIX (0 .. 100).
//   With shift held, the four knobs turn values kept with the sound (level 7, `veffgraindelay_grains`):
//   DELAY (the notes 2/1 .. 1/64 of the tempo, then 0 ms, then 1 .. 1000 ms), FREQ (the grain rate, 1 .. 150 Hz),
//   RANDOM (random pitch of each grain, 0 .. +-12 semitones), SPREAD (each grain's random place between left and
//   right); shift and a press of the 4th knob is REVERSE (every grain read backward).
//   A press of the 1st knob puts the pitch to 0, of the 2nd the spray to 0, of the 3rd and 4th the feedback and the
//   mix to their start. With shift, a press of the 1st, 2nd or 3rd puts the delay, the freq or the random to its start.
//   It starts as an octave-up shimmer on the eighth note.
//
// The sound: the input plus the feedback goes into a stereo line (one word a frame: left in the high half, right in
// the low half). Grains read the line: one starts every launch period (1 / FREQ), lasts two periods, so two overlap
// and their windows (a smoothstep up and down) add to one. Each grain is given at its start a read place (DELAY back,
// plus SPRAY at random, plus what a grain pitched up needs so it never passes the write place), a rate (PITCH plus
// RANDOM), a side (SPREAD) and a direction (REVERSE). The grains' sum is the wet; the wet, through a high-pass at
// ~20 Hz and a soft knee, times FEEDBACK, goes back into the line.
// On the audio thread: no division, no 64-bit multiply, no float; every sum and difference held at full scale.

#include "build_r6e_card.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"                       // veff_ui.h (common, unedited)
#include "veff_ui.h"
#pragma GCC diagnostic pop
#include "graindelay_tables.h"

typedef unsigned int u32;
typedef int i32;
typedef short i16;

enum { ASK_EFFECT = 0, ASK_KEEPS = 1, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { BLOCK = 64, GRAINS = 3 };                                      // two overlap; a third a moment after the freq goes up
enum { V_PITCH = 0, V_SPRAY = 1, V_FEEDBACK = 2, V_MIX = 3 };
enum { E_DELAY = 0, E_FREQ = 1, E_RANDOM = 2, E_SPREAD = 3, E_REVERSE = 4, EXTRAS = 5 };
enum { LINE_BITS = 17, LINE = 1 << LINE_BITS, LINE_MASK = LINE - 1 };  // 2.97 s of stereo line, 512 KB
enum { DELAY_LEAST = 2 * BLOCK, DELAY_MOST = 88200, SPRAY_MOST_MS = 500, SPRAY_MOST = 22050, MARGIN = 2 * BLOCK + 16 };
enum { NOTES = 8, MS_STEPS = 85, DELAY_STEPS = NOTES + MS_STEPS };
enum { PLACES = 17 };                                                  // the sounds' slots 0..15 and the master
enum { PITCH_MOST_CENTS = 2400, RANDOM_CENTS_PER = 12 };               // random 100: +-1200 cents
static const u32 BEATS_Q8[NOTES] = { 2048, 1024, 512, 256, 128, 64, 32, 16 };   // 2/1 .. 1/64 of a 4/4 bar, in beats * 256
static const char* const NOTE_NAMES[NOTES] = { "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64" };
static const int EXTRA_START[EXTRAS] = { 4, 34, 0, 40, 0 };            // 1/8, ~16 Hz, no random, some spread, forward
static const int EXTRA_MOST[EXTRAS] = { DELAY_STEPS - 1, FREQS - 1, 100, 100, 1 };
static const int VALUE_START[4] = { 11585, 15, 45, 35 };               // +12 st, 15 ms, 45 %, 35 % (as the recipe)
static const char KEY[] = "veffgraindelay_grains";

extern "C" const Op1fCard op1f_card_veffgraindelay OP1F_NAMED("op1f_card_veffgraindelay");

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

static inline int held(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------- the values past the four knobs
// Kept by place (0..15 a sound's slot, 16 the master), written by the frame function, read by process: single words.
// A place run inside another pack (fx train's wagons) plays the start values.
static volatile int extra[PLACES][EXTRAS];
static volatile u32 extra_ready;                                       // bit n: place n's values were read

static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- the sound (audio thread)
struct Grain {
    u32 pos;                                                           // the read place, frames * 2^15 (wraps with the line)
    i32 rate;                                                          // frames * 2^15 a frame; below 0 backward
    u32 ph, inc;                                                       // the window's phase: 0 .. 2^32 over the grain
    i32 gl, gr;                                                        // the side: Q31 gains, one of them 1 (used as Q15)
    u32 live;
};
struct State {
    Grain g[GRAINS];
    u32 wpos, filled, launch, rng;                                     // launch: the launch period's phase
    i32 mix, hl, hr;                                                   // hl, hr: the feedback's high-pass (its low part)
    i32 wl[BLOCK], wr[BLOCK];                                          // the block's wet, at a quarter scale
    u32 line[LINE];                                                    // left << 16 | right, 16 bits each
};
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 524928, "the recipe's state_bytes");

static volatile u32 level;                                             // the last block's wet peak, for the page
enum { SNAPS = 64 };                                                   // the page's envelope: one input sample a call
static volatile i32 snap[SNAPS];
static volatile u32 snap_at;
// the grains for the page: each slot's distance behind the write place (frames), window (0..256), rate (Q15)
static volatile u32 view_back[GRAINS], view_win[GRAINS];
static volatile i32 view_rate[GRAINS];
static volatile u32 view_tick;                                         // frames played: the page's tape moves with it
static volatile u32 heard_period = 60214886u;                          // the tempo's period, as the last block had it (120 bpm first)

static inline i32 soft(i32 x) { return subq(x, mulq(mulq(x, x), x) >> 2); }   // the feedback's knee: x - x^3/4

static inline u32 random(u32& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }   // xorshift32

// PITCH: -16383 .. 16383, squared to -2400 .. +2400 cents
static inline int pitch_cents(int v) {
    const int x = held(v, -16383, 16383);
    const int a = x < 0 ? -x : x;
    const i32 q = (i32)((u32)a << 17);
    const int c = mulq(mulq(q, q), PITCH_MOST_CENTS);
    return x < 0 ? -c : c;
}

// 2^(cents / 1200) in Q15, cents held to -2400 .. +2400: 0.25 .. 4.0
static inline i32 rate_of(int cents) {
    u32 c = (u32)(held(cents, -PITCH_MOST_CENTS, PITCH_MOST_CENTS) + PITCH_MOST_CENTS);   // 0 .. 4800
    u32 oct = 0;
    while (c >= 1200u) { c -= 1200u; ++oct; }
    const u32 i = c >> 4, f = c & 15u;
    const i32 t = EXP2_16C[i] + (((EXP2_16C[i + 1] - EXP2_16C[i]) * (i32)f) >> 4);   // 32768 .. 65536
    return (i32)(((u32)t << oct) >> 2);
}

static u32 delay_frames(int step, u32 period) {
    const int s = held(step, 0, DELAY_STEPS - 1);
    u32 q;
    if (s < NOTES) {
        u32 p = period >> 6;                                           // a 96th of a beat, frames * 2^12
        if (p > 40000000u) p = 40000000u;
        const u32 beat = (p * 96u) >> 12;                              // frames a beat
        q = (beat * BEATS_Q8[s]) >> 8;
    } else q = ((u32)step_ms(s - NOTES) * 11290u) >> 8;                 // 44.1 frames a ms
    return q < (u32)DELAY_LEAST ? (u32)DELAY_LEAST : q > (u32)DELAY_MOST ? (u32)DELAY_MOST : q;
}

// How far one grain of `len` frames at `rate` reaches past its start, backward or forward.
static inline u32 reach(i32 rate, u32 len, int reverse) {
    const u32 r = reverse ? (u32)rate : rate > 32768 ? (u32)(rate - 32768) : 0u;
    if (r == 0u) return 0u;
    return ((r >> 5) * len >> 10) + (len >> 9) + 4u;                   // rounded up
}

// Everything a block needs, worked out once.
struct Plan {
    i32 base_cents, random_cents, spread, fb, want;
    u32 delay, spray, inc, len;
    int reverse, freq;
};

static void plan_of(Plan& p, const int* v, const volatile int* e, u32 period) {
    const int ex[EXTRAS] = { e ? e[0] : EXTRA_START[0], e ? e[1] : EXTRA_START[1], e ? e[2] : EXTRA_START[2],
                             e ? e[3] : EXTRA_START[3], e ? e[4] : EXTRA_START[4] };
    p.base_cents = pitch_cents(v[V_PITCH]);
    p.random_cents = held(ex[E_RANDOM], 0, 100) * RANDOM_CENTS_PER;
    p.spread = held(ex[E_SPREAD], 0, 100) >= 100 ? 0x7fffffff : held(ex[E_SPREAD], 0, 100) * 21474836;
    p.fb = held(v[V_FEEDBACK], 0, 95) * 21474836;                      // hundredths in Q31
    p.want = held(v[V_MIX], 0, 100) >= 100 ? 0x7fffffff : held(v[V_MIX], 0, 100) * 21474836;
    p.reverse = ex[E_REVERSE] & 1;
    p.delay = delay_frames(ex[E_DELAY], period);
    p.spray = ((u32)held(v[V_SPRAY], 0, SPRAY_MOST_MS) * 11290u) >> 8;
    if (p.spray > (u32)SPRAY_MOST) p.spray = SPRAY_MOST;
    // the longest grain the line holds at the fastest rate this block can give: shorter (a higher freq) when not
    const i32 top = rate_of(p.base_cents + p.random_cents);            // the fastest rate a grain can have
    const u32 room = (u32)LINE - (u32)MARGIN - p.delay - p.spray;
    int f = held(ex[E_FREQ], 0, FREQS - 1);
    while (f < FREQS - 1 && reach(top, FREQ_LEN[f], p.reverse) > room) ++f;
    p.freq = f;
    p.inc = FREQ_INC[f];
    p.len = FREQ_LEN[f];
}

static Grain* launch(State& st, const Plan& p, u32 at, u32 ph) {
    Grain* g = 0;
    for (int k = 0; k < GRAINS; ++k) if (!st.g[k].live) { g = &st.g[k]; break; }
    if (!g) return 0;                                                    // all three busy (the freq just went up): skip one
    const u32 r1 = random(st.rng), r2 = random(st.rng), r3 = random(st.rng);
    const u32 sprayed = ((r1 >> 16) * p.spray) >> 16;
    int cents = p.base_cents;
    if (p.random_cents) cents += mulq((i32)r2, p.random_cents);       // -random .. +random
    const i32 rate = rate_of(cents);
    u32 back = p.delay + sprayed;
    if (!p.reverse) back += reach(rate, p.len, 0);                     // pitched up: start further back
    const u32 far = p.reverse ? back + reach(rate, p.len, 1) : back;   // the oldest frame it reads
    if (far + 2u > st.filled + at) return 0;                           // older than a fresh start: silence, no grain
    g->pos = ((st.wpos + at - back) & (u32)LINE_MASK) << 15;
    g->rate = p.reverse ? -rate : rate;
    g->ph = ph;
    g->inc = p.inc;
    const i32 side = mulq((i32)r3, p.spread);                          // -spread .. +spread
    g->gl = side > 0 ? subq(0x7fffffff, side) : 0x7fffffff;
    g->gr = side < 0 ? addq(0x7fffffff, side) : 0x7fffffff;
    g->live = 1;
    return g;
}

// One grain over frames [from, BLOCK): read, window, side, added into the block's wet at a quarter scale. Every product is
// a 16-bit one in a 32-bit word (the samples are 16 bits, the window and the sides Q15), so no Q31 multiply here.
static void render(Grain& g, const u32* line, i32* wl, i32* wr, u32 from) {
    u32 pos = g.pos, ph = g.ph;
    const i32 rate = g.rate, gl = g.gl >> 16, gr = g.gr >> 16;        // the sides in Q15
    const u32 inc = g.inc;
    const int sided = (gl & gr) != 0x7fff;
    for (u32 n = from; n < (u32)BLOCK; ++n) {
        const u32 i = pos >> 15;
        const i32 frac = (i32)(pos & 0x7fffu);
        const u32 a = line[i], b = line[(i + 1u) & (u32)LINE_MASK];   // two neighbours, one word each for both sides
        const i32 al = (i32)a >> 16, bl = (i32)b >> 16, ar = (i16)a, br = (i16)b;
        const i32 yl = al + (((bl - al) * frac) >> 15);                // 16 bits; (bl - al) * frac < 2^31
        const i32 yr = ar + (((br - ar) * frac) >> 15);
        const i32 t = (i32)((ph < 0x80000000u ? ph : ~ph) >> 16);      // 0 .. 32767 and back; the twin grain has 32767 - t
        const i32 t2 = (t * t) >> 15, t3 = (t2 * t) >> 15;
        const i32 w = 3 * t2 - 2 * t3;                                 // smoothstep, Q15: w(t) + w(1 - t) = 1
        i32 kl = w, kr = w;
        if (sided) { kl = (w * gl) >> 15; kr = (w * gr) >> 15; }
        wl[n] += (yl * kl) >> 1;                                       // 16 bits times Q15, halved: Q31 at a quarter scale,
        wr[n] += (yr * kr) >> 1;                                       // so the three grains never overflow
        pos += (u32)rate;
        const u32 nph = ph + inc;
        if (nph < ph) { g.live = 0; return; }                          // the window is through
        ph = nph;
    }
    g.pos = pos; g.ph = ph;
}

static inline i32 to_line(i32 x) { return (x < 0 ? x + 0xffff : x) >> 16; }   // 16 bits, rounded toward zero

static void clear_small(State& st) {                                   // what a fresh start forgets: not the line
    u32* w = (u32*)&st;
    const u32 words = (u32)((const char*)&st.line[0] - (const char*)&st) >> 2;
    for (u32 i = 0; i < words; ++i) w[i] = 0;
    st.rng = 0x9e3779b9u;
}

extern "C" void veffgraindelay_process(const Op1fEffectBlock* now) OP1F_NAMED("veffgraindelay_process");
extern "C" void veffgraindelay_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    if (now->fresh || st.rng == 0) clear_small(st);
    { const u32 at = snap_at; snap[at & (SNAPS - 1)] = now->left[0]; snap_at = at + 1; }   // for the page only
    Plan p;
    plan_of(p, now->values, extras_of(now->place), now->period);
    const i32 from = st.mix;
    if (!now->on || (p.want == 0 && from == 0) || now->frames < (u32)BLOCK) {   // off, or mix 0: the sound as it came
        st.filled = 0;                                                 // the line is not written: forget it, and the grains
        for (int k = 0; k < GRAINS; ++k) st.g[k].live = 0;
        return;
    }
    const i32 step = (p.want >> 6) - (from >> 6);
    int* const left = now->left;
    int* const right = now->right;
    i32* const wl = st.wl;
    i32* const wr = st.wr;
    for (u32 n = 0; n < (u32)BLOCK; ++n) { wl[n] = 0; wr[n] = 0; }

    // the launch: at most one a block (a launch period is 294 frames or more)
    const u32 twice = p.inc * 2u;
    u32 ln = st.launch, at = (u32)BLOCK;
    for (u32 n = 0; n < (u32)BLOCK; ++n) {
        const u32 next = ln + twice;
        if (next < ln && at == (u32)BLOCK) at = n;
        ln = next;
    }
    const u32 start_ph = (u32)(st.launch + twice * (at + 1u)) >> 1;    // the new grain's phase: the launch's past 0, halved
    st.launch = ln;
    for (int k = 0; k < GRAINS; ++k) if (st.g[k].live) render(st.g[k], st.line, wl, wr, 0);
    if (at < (u32)BLOCK) {
        Grain* g = launch(st, p, at, start_ph);
        if (g) render(*g, st.line, wl, wr, at);
    }

    // the line, the feedback and the mix, frame by frame
    u32 w = st.wpos;
    i32 m = from, peak = 0, hl = st.hl, hr = st.hr;
    const i32 fb = p.fb;
    for (u32 n = 0; n < (u32)BLOCK; ++n) {
        m += step;
        const i32 dl = left[n], dr = right[n];
        i32 xl = addq(wl[n], wl[n]), xr = addq(wr[n], wr[n]);          // the wet at full scale (held: three
        xl = addq(xl, xl); xr = addq(xr, xr);                          // grains a moment after the freq went up)
        hl = addq(hl, mulq(subq(xl, hl), 6290000));                    // ~20 Hz
        hr = addq(hr, mulq(subq(xr, hr), 6290000));
        const i32 il = addq(dl, mulq(fb, soft(subq(xl, hl))));
        const i32 ir = addq(dr, mulq(fb, soft(subq(xr, hr))));
        st.line[w] = ((u32)to_line(il) << 16) | ((u32)to_line(ir) & 0xffffu);
        w = (w + 1u) & (u32)LINE_MASK;
        const i32 keep = 0x7fffffff - m;
        left[n] = addq(mulq(keep, dl), mulq(m, xl));
        right[n] = addq(mulq(keep, dr), mulq(m, xr));
        const i32 ml = xl < 0 ? (xl == (i32)0x80000000 ? 0x7fffffff : -xl) : xl;
        const i32 mr = xr < 0 ? (xr == (i32)0x80000000 ? 0x7fffffff : -xr) : xr;
        if (ml > peak) peak = ml;
        if (mr > peak) peak = mr;
    }
    st.hl = hl; st.hr = hr;
    st.wpos = w;
    st.filled = st.filled + (u32)BLOCK < (u32)LINE ? st.filled + (u32)BLOCK : (u32)LINE;
    st.mix = p.want;
    level = (u32)peak;
    for (int k = 0; k < GRAINS; ++k) {                                 // for the page only
        const Grain& g = st.g[k];
        const u32 tri = g.ph < 0x80000000u ? g.ph : ~g.ph;
        view_win[k] = g.live ? tri >> 23 : 0u;
        view_back[k] = (w - (g.pos >> 15)) & (u32)LINE_MASK;
        view_rate[k] = g.rate;
    }
    view_tick = view_tick + (u32)BLOCK;
    heard_period = now->period;
}

extern "C" void veffgraindelay_reset(unsigned place, void* state) OP1F_NAMED("veffgraindelay_reset");
extern "C" void veffgraindelay_reset(unsigned place, void* state) {
    (void)place;
    if (state == 0) return;
    // everything but the line (~1.5 KB, not 512 KB): with filled 0 no grain reads the line before it is written
    clear_small(*(State*)state);
}

// ---------------------------------------------------------------- told by the base (display thread): single words
static volatile u32 shown_where, shown_how;
static volatile u32 reread;                                            // bit n: read place n's kept values again

extern "C" void veffgraindelay_shown(unsigned where, unsigned how) OP1F_NAMED("veffgraindelay_shown");
extern "C" void veffgraindelay_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffgraindelay_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffgraindelay_changed");
extern "C" void veffgraindelay_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread): single words
static volatile int pending_turn[4];                                   // shift-held turns of the four knobs, in detents
static volatile u32 pending_click;                                     // bits 0..3: a knob pressed; 4..7: with shift

extern "C" int veffgraindelay_input(const Op1fInput* e) OP1F_NAMED("veffgraindelay_input");
extern "C" int veffgraindelay_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {         // shift held: the kept values are ours
        const int steps = e->turn >> 10;
        if (steps) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        if (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) pending_click |= 16u << knob;
        else pending_click |= 1u << knob;
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
static char* put_tenths(char* p, int t, int sign) {                    // +12.0, -0.5, 7.5
    if (sign) p = put(p, t < 0 ? "-" : "+");
    u32 n = (u32)(t < 0 ? -t : t), whole = 0;
    while (n >= 10u) { n -= 10u; ++whole; }
    p = put_number(p, (int)whole);
    p = put(p, ".");
    return put_number(p, (int)n);
}

static void extras_start(u32 p) { for (int k = 0; k < EXTRAS; ++k) extra[p][k] = EXTRA_START[k]; }

// Our own form only: {"d":4,"f":34,"r":0,"s":40,"v":1,"x":0}
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

static const char NAMES[EXTRAS] = { 'd', 'f', 'r', 's', 'x' };

static void extras_read(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffgraindelay, where, which, KEY, text, sizeof text) > 0) {
        text[sizeof text - 1] = 0;
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
    q = put(q, ",\"f\":"); q = put_number(q, extra[p][E_FREQ]);
    q = put(q, ",\"r\":"); q = put_number(q, extra[p][E_RANDOM]);
    q = put(q, ",\"s\":"); q = put_number(q, extra[p][E_SPREAD]);
    q = put(q, ",\"v\":1,\"x\":"); q = put_number(q, extra[p][E_REVERSE]); put(q, "}");
    if (op1f_value_set(&op1f_card_veffgraindelay, where, which, KEY, text) == 0) unsaved &= ~(1u << p);   // else again next frame
}

// ---------------------------------------------------------------- the page: the common Veff page (veff_ui.h)
// Painted every frame on the base's blank page. The centre picture: the delay line along the axis, now at the right,
// older to the left, its tape ticks moving left as the sound is written. A dashed mark stands at DELAY behind now, a
// pink bracket under the axis spans SPRAY. Each grain playing is a pink bell over the place it reads: as wide as the
// grain is long, as tall as its window now and the input's envelope; a white stroke from its foot leans the way its
// pitch moves it (right: up, toward now; left: down, or REVERSE). Fainter white echoes of each grain stand a DELAY
// further back for each turn of the FEEDBACK. Integer maths only.
static struct { u32 asked; int env, lvl; char line[4][16]; } f;

using namespace veffui;
enum { ART_L = VEFF_AXIS_L, ART_R = VEFF_AXIS_R, ART_W = VEFF_AXIS_R - VEFF_AXIS_L, BELL_PTS = 16 };

static void bell(void* c, int cx, int half, int h, unsigned col) {      // a smoothstep bump, 2 * half wide, h tall
    int px = cx - half, py = VEFF_AXIS_Y;
    for (int i = 1; i <= BELL_PTS; ++i) {
        const int t = i <= BELL_PTS / 2 ? i : BELL_PTS - i;            // 0 .. 8 .. 0
        const int u = t << 5;                                          // 0..256
        const int s = (u * u * (768 - 2 * u)) >> 16;                   // 3u^2 - 2u^3, 0..256
        const int qx = cx - half + ((2 * half * i) >> 4);
        const int qy = VEFF_AXIS_Y - ((h * s) >> 8);
        op1f_paint_line(c, px, py, qx, qy, 1, col);
        px = qx; py = qy;
    }
}

static inline int x_of(u32 back, int span_sh) {
    if (back >= ((u32)1 << span_sh)) return ART_L;
    return ART_R - (int)((back * (u32)ART_W) >> span_sh);
}

static void paint_art(void* c, const int* v, const int* x, u32 period) {
    using namespace veffui;
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
    Plan p;
    plan_of(p, v, x, period);
    // the span shown: a power of two of frames, at least what the grains reach and a quarter more, so a place's x is
    // a multiply and a shift
    const u32 need = p.delay + p.spray + reach(rate_of(p.base_cents + p.random_cents), p.len, p.reverse);
    u32 sh = 10;
    while (sh < (u32)LINE_BITS && ((u32)1 << sh) < need + (need >> 2)) ++sh;
    const int span_sh = (int)sh;
    const int mix = held(v[V_MIX], 0, 100), fb = held(v[V_FEEDBACK], 0, 95);
    const int bright = 112 + ((mix * 368) >> 8);                       // 112..256
    const int glow = held(bright + f.lvl, 0, 256);

    axis(c);
    {   // the tape: ticks every 1/16 of the span, moving left as frames are written
        const u32 gap = (u32)1 << (span_sh - 4), off = view_tick & (gap - 1u);
        for (u32 b = off; b < ((u32)1 << span_sh); b += gap) {
            const int tx = x_of(b, span_sh);
            op1f_paint_line(c, tx, VEFF_AXIS_Y + 2, tx, VEFF_AXIS_Y + 5, 1, white(96));
        }
    }
    // now: the write place, its dot swelling with the input
    op1f_paint_line(c, ART_R, VEFF_AXIS_Y - 30, ART_R, VEFF_AXIS_Y + 8, 1, VEFF_PINK);
    disc(c, ART_R, VEFF_AXIS_Y, 2 + ((f.env * 4) >> 8), VEFF_PINK);
    // DELAY: dashed; SPRAY: a bracket under the axis from DELAY back
    const int dx = x_of(p.delay, span_sh);
    dashed_v(c, dx, VEFF_AXIS_Y - VEFF_ART_HALF, VEFF_AXIS_Y + 12, VEFF_WHITE, 4, 3);
    if (p.spray) {
        const int sx = x_of(p.delay + p.spray, span_sh);
        const int by = VEFF_AXIS_Y + 14;
        op1f_paint_line(c, sx, by, dx, by, 1, pink(bright));
        op1f_paint_line(c, sx, by - 3, sx, by, 1, pink(bright));
        op1f_paint_line(c, dx, by - 3, dx, by, 1, pink(bright));
    }
    // the grains, and their echoes a DELAY further back each, fainter with the feedback
    const int half = held((int)((p.len * (u32)ART_W) >> (span_sh + 1)), 4, 60);
    const int tall = 20 + ((f.env * 26) >> 8);                         // up to 46: within VEFF_ART_HALF
    for (int k = 0; k < GRAINS; ++k) {
        const int win = (int)view_win[k];
        if (!win) continue;
        const u32 back = view_back[k];
        const int gx = x_of(back, span_sh);
        const int s = (win * win * (768 - 2 * win)) >> 16;              // the window's smoothstep, 0..256
        const int h = (tall * (40 + s)) >> 8 > tall ? tall : (tall * (40 + s)) >> 8;
        bell(c, gx, half, h, pink((glow * (96 + ((s * 5) >> 3))) >> 8));
        int lean = (int)((view_rate[k] - 32768) >> 11);                 // the pitch: right for up, left for down
        lean = held(lean, -36, 36);
        op1f_paint_line(c, gx, VEFF_AXIS_Y, gx + lean, VEFF_AXIS_Y - h, 1, VEFF_WHITE);
        int alpha = (bright * fb * 3) >> 8;
        u32 eb = back;
        for (int e = 0; e < 3 && alpha > 24; ++e) {
            eb += p.delay;
            if (eb >= ((u32)1 << span_sh)) break;
            bell(c, x_of(eb, span_sh), half, h >> 1, white(alpha));
            alpha = (alpha * fb * 3) >> 8;
        }
    }
    sparkles(c);
}

static void icon_grains(void* c, unsigned colour) {                    // three little bells at the icons' place
    const int base = 32;                                               // the icons' box: y 10 .. 34
    for (int k = 0; k < 3; ++k) {
        static const int CX[3] = { 438, 452, 466 }, H[3] = { 10, 20, 7 };
        int px = CX[k] - 8, py = base;
        for (int i = 1; i <= 8; ++i) {
            const int t = i <= 4 ? i : 8 - i, u = t << 6;
            const int s = (u * u * (768 - 2 * u)) >> 16;
            const int qx = CX[k] - 8 + 2 * i, qy = base - ((H[k] * s) >> 8);
            op1f_paint_line(c, px, py, qx, qy, 1, colour);
            px = qx; py = qy;
        }
    }
}

static void paint(void* canvas, u32 where, int shift, int on, u32 period) {
    int v[4];
    for (u32 i = 0; i < 4u; ++i) if (op1f_param_heard(&op1f_card_veffgraindelay, ASK_EFFECT, where, i, &v[i]) != 0) v[i] = VALUE_START[i];
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
    veffui::Column col[4];
    char (*t)[16] = f.line;
    if (!shift) {
        const int cents = pitch_cents(v[V_PITCH]);
        const int tenths = (int)(((u32)(cents < 0 ? -cents : cents) * 6554u + 32768u) >> 16);   // cents / 10, rounded
        put(put_tenths(t[0], cents < 0 ? -tenths : tenths, 1), " st");
        put(put_number(t[1], held(v[V_SPRAY], 0, SPRAY_MOST_MS)), " ms");
        put(put_number(t[2], held(v[V_FEEDBACK], 0, 95)), "%");
        put(put_number(t[3], held(v[V_MIX], 0, 100)), "%");
        col[0] = { "PITCH", t[0], (held(v[V_PITCH], -16383, 16383) + 16384) >> 5 };
        col[1] = { "SPRAY", t[1], (held(v[V_SPRAY], 0, SPRAY_MOST_MS) * 2097) >> 10 };
        col[2] = { "FEEDBACK", t[2], (held(v[V_FEEDBACK], 0, 95) * 11038) >> 10 };
        col[3] = { "MIX", t[3], (held(v[V_MIX], 0, 100) * 10486) >> 10 };
    } else {
        const int st = held(x[E_DELAY], 0, DELAY_STEPS - 1);
        if (st < NOTES) put(t[0], NOTE_NAMES[st]); else put(put_number(t[0], step_ms(st - NOTES)), " ms");
        put(put_tenths(t[1], FREQ_TENTHS[held(x[E_FREQ], 0, FREQS - 1)], 0), " hz");
        put(put_number(t[2], held(x[E_RANDOM], 0, 100)), "%");
        put(put_number(t[3], held(x[E_SPREAD], 0, 100)), x[E_REVERSE] ? "% r" : "%");
        col[0] = { "DELAY", t[0], (st * 11134) >> 10 };
        col[1] = { "FREQ", t[1], (held(x[E_FREQ], 0, FREQS - 1) * 16644) >> 10 };
        col[2] = { "RANDOM", t[2], (held(x[E_RANDOM], 0, 100) * 10486) >> 10 };
        col[3] = { x[E_REVERSE] ? "SPREAD REV" : "SPREAD", t[3], (held(x[E_SPREAD], 0, 100) * 10486) >> 10 };
    }
    paint_art(canvas, v, x, period);
    veffui::page(canvas, "GRAIN DELAY", -1, col, shift ? "SHIFT" : 0, on);  // no icon of veff_ui.h's: ours below
    icon_grains(canvas, VEFF_WHITE);
}

extern "C" void veffgraindelay_frame(const Op1fFrame* now) OP1F_NAMED("veffgraindelay_frame");
extern "C" void veffgraindelay_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffgraindelay, "veffgraindelaypage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffgraindelay, ASK_EFFECT, page_doc);
    }
    if (!f.asked) {
        f.asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
    }
    // the kept values: read again where the base said they changed, or the first time a place rides us
    const u32 riding = (u32)op1f_riding(&op1f_card_veffgraindelay, ASK_EFFECT);
    u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    // the page's own keys: shift-held turns and presses
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        for (int k = 0; k < 4; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            extra[where][k] = held(extra[where][k] + t * (k >= 2 ? 2 : 1), 0, EXTRA_MOST[k]);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 1u) op1f_param_set(&op1f_card_veffgraindelay, ASK_EFFECT, where, V_PITCH, 0);
        if (clicks & 2u) op1f_param_set(&op1f_card_veffgraindelay, ASK_EFFECT, where, V_SPRAY, 0);
        if (clicks & 4u) op1f_param_set(&op1f_card_veffgraindelay, ASK_EFFECT, where, V_FEEDBACK, VALUE_START[V_FEEDBACK]);
        if (clicks & 8u) op1f_param_set(&op1f_card_veffgraindelay, ASK_EFFECT, where, V_MIX, VALUE_START[V_MIX]);
        for (int k = 0; k < 3; ++k) if (clicks & (16u << k)) { extra[where][k] = EXTRA_START[k]; unsaved |= 1u << where; }
        if (clicks & 128u) { extra[where][E_REVERSE] ^= 1; unsaved |= 1u << where; }
    } else {
        for (int k = 0; k < 4; ++k) pending_turn[k] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffgraindelay && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP))
        paint(now->canvas, where, now->shift != 0, !(how & (u32)OP1F_SHOWN_OFF), heard_period);
}
