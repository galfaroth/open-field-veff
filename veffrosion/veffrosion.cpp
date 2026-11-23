// veffrosion: a degrader in the spirit of Ableton's Erosion, as an effect of the unit's own lists (level 8 of
// open-field's base, level 7 for the values past the four knobs, level 10 for the page). Our own implementation from
// the behaviour. It changes no place of the firmware and names no address of it.
//
// What the owner meets:
//   `veffrosion` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer) and on every
//   wagon of fx train. It stands on the unit's `delay`.
//   Knobs: FREQ (30 Hz .. 18 kHz, 128 log steps), WIDTH (0 .. 100, the noise's band), AMOUNT (0 .. 100, the depth of
//   the delay's wobble), MIX (0 .. 100). With shift held (level 7, `veffrosion_mode`): MODE (sine, noise, wide) on the
//   1st knob, TONE (a low-pass on the grit) on the 2nd. A press of the 1st knob puts FREQ back to 1 kHz, of the 4th
//   puts MIX to 0.
//
// The sound, each side: a delay line of 64 frames; read at 0.5 ms + amount x mod(t) (linear interpolation, at most
// +-0.45 ms) and at 0.5 ms still. mod is a sine at FREQ, or white noise through a band-pass at FREQ (a zero-delay
// state-variable filter, coefficients and loudness from a table), one generator for both sides (noise) or one each
// (wide). The grit is the wobbling read minus the still one: the sidebands alone, no static comb. Out is the dry
// sound whole plus the grit (through TONE) x MIX. So MIX 0 is the sound bit for bit, and the dry is always kept.
// On the audio thread: no division, no 64-bit multiply, no float; every sum held at full scale.

#include "build_r6e_card.h"
#include "veff_ui.h"
#include "erosion_tables.h"

typedef unsigned int u32;
typedef int i32;

enum { ASK_EFFECT = 0, ASK_KEEPS = 1, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { CHANNELS = 2, BLOCK = 64, LINE = 64, LINE_MASK = LINE - 1 };
enum { V_FREQ = 0, V_WIDTH = 1, V_AMOUNT = 2, V_MIX = 3 };
enum { E_MODE = 0, E_TONE = 1, EXTRAS = 2 };
enum { MODE_SINE = 0, MODE_NOISE = 1, MODE_WIDE = 2 };
enum { BASE = 22, BASE_Q8 = BASE * 256, DEPTH_PER_PCT = 51, FREQ_START = 70 };   // 0.5 ms; 100 % is 19.9 frames
enum { PLACES = 17 };
static const int EXTRA_START[EXTRAS] = { MODE_NOISE, 70 };
static const int EXTRA_MOST[EXTRAS] = { 2, 100 };
static const char* const MODE_NAMES[3] = { "sine", "noise", "wide" };
static const char KEY[] = "veffrosion_mode";

extern "C" const Op1fCard op1f_card_veffrosion OP1F_NAMED("op1f_card_veffrosion");

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
static inline i32 subq(i32 a, i32 b) { return __builtin_bfin_sub_fr1x32(a, b); }   // never -x
#endif

static inline int held(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline i32 shl_held(i32 x, int sh) {                            // x << sh, held at full scale
    const i32 lim = 0x7fffffff >> sh;
    return x > lim ? 0x7fffffff : x < -lim ? (i32)0x80000001 : x << sh;
}

// ---------------------------------------------------------------- the values past the four knobs
static volatile int extra[PLACES][EXTRAS];
static volatile u32 extra_ready;
static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- told by the base (display thread)
static volatile u32 shown_where, shown_how;

// ---------------------------------------------------------------- the page's feed (written by the audio thread)
// The shown place's input: a peak of 8 samples a call, and a snapshot of 48 samples (every 8th frame, 6 calls), asked
// for by the frame (snap_fill 0) and done at 48. Plain 32-bit stores; torn reads only make the picture wrong for a frame.
enum { SNAP = 48 };
static volatile i32 snap[SNAP];
static volatile u32 snap_fill = SNAP, peak_word;

// ---------------------------------------------------------------- the sound (audio thread)
struct Gen { u32 seed; i32 ic1, ic2; };
struct State {
    i32 line[CHANNELS][LINE];                                          // the sound >> 2
    Gen gen[CHANNELS];
    i32 lp[CHANNELS];
    u32 wpos, phase;
    i32 mix, depth;
};
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 576, "the recipe's state_bytes");

static inline i32 sine(u32 phase) {
    const u32 i = phase >> 22;
    const i32 f = (i32)((phase >> 6) & 0xffffu) << 15;
    const i32 s0 = SINE[i], s1 = SINE[i + 1];
    return s0 + mulq((s1 >> 1) - (s0 >> 1), f) * 2;
}

static inline i32 noise_bp(Gen* g, const Svf* c) {
    u32 x = g->seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->seed = x;
    const i32 v0 = (i32)x >> 4;                                        // white, +-2^27
    const i32 v3 = subq(v0, g->ic2);
    const i32 v1 = addq(mulq(c->a1, g->ic1), mulq(c->a2, v3));
    const i32 v2 = addq(g->ic2, mulq(c->a3, v3));
    g->ic1 = subq(addq(v1, v1), g->ic1);
    g->ic2 = subq(addq(v2, v2), g->ic2);
    return shl_held(mulq(v1, c->make), c->shift);
}

static void clear(State& st) {
    u32* w = (u32*)&st;
    for (u32 i = 0; i < sizeof(State) / 4; ++i) w[i] = 0;
    st.gen[0].seed = 0x9e3779b9u; st.gen[1].seed = 0x7f4a7c15u;
}

extern "C" void veffrosion_process(const Op1fEffectBlock* now) OP1F_NAMED("veffrosion_process");
extern "C" void veffrosion_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    if (now->fresh || st.gen[0].seed == 0) clear(st);
    const int* v = now->values;
    const volatile int* e = extras_of(now->place);
    const int mode = held(e ? e[E_MODE] : EXTRA_START[E_MODE], 0, 2);
    const int tone_pct = held(e ? e[E_TONE] : EXTRA_START[E_TONE], 0, 100);
    const int fi = held(v[V_FREQ], 0, FREQ_STEPS - 1);
    const int wl = ((held(v[V_WIDTH], 0, 100) + 5) * 205) >> 11;       // 0 .. 10
    const Svf* coef = &SVF[fi * WIDTH_LEVELS + wl];
    const u32 inc = FREQ_INC[fi];
    const int mixp = held(v[V_MIX], 0, 100);
    const i32 want = mixp >= 100 ? 0x7fffffff : mixp * 21474836;
    const i32 depth_want = held(v[V_AMOUNT], 0, 100) * DEPTH_PER_PCT;
    const i32 t = tone_pct >= 100 ? 0x7fffffff : tone_pct * 21474836;
    const i32 tone = 0x08000000 + mulq(t, t) - mulq(0x08000000, t);    // ~450 Hz .. open
    const i32 from = st.mix;
    if (now->on && now->place == shown_where) {                        // the page's feed: about 150 cycles
        const int* in = now->left;
        const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
        u32 pk = 0, f = snap_fill;
        for (u32 n = 0; n < frames; n += 8u) {
            const i32 x = in[n];
            const u32 a = (u32)(x ^ (x >> 31));                        // |x|, never -x
            if (a > pk) pk = a;
            if (f < (u32)SNAP) snap[f++] = x;
        }
        snap_fill = f;
        peak_word = pk;
    }
    if (!now->on || (want == 0 && from == 0)) return;                  // off, or mix 0: the sound as it came
    const i32 step = (want >> 6) - (from >> 6);
    const i32 dfrom = st.depth, dstep = (depth_want - dfrom) >> 6;
    int* bus[CHANNELS] = { now->left, now->right };
    const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    i32 m = from, depth = dfrom;
    u32 w = st.wpos;
    for (u32 n = 0; n < frames; ++n) {
        m += step; depth += dstep;
        i32 mod[CHANNELS];
        if (mode == MODE_SINE) { mod[0] = mod[1] = sine(st.phase); st.phase += inc; }
        else if (mode == MODE_NOISE) mod[0] = mod[1] = noise_bp(&st.gen[0], coef);
        else { mod[0] = noise_bp(&st.gen[0], coef); mod[1] = noise_bp(&st.gen[1], coef); }
        for (int c = 0; c < CHANNELS; ++c) {
            i32* line = st.line[c];
            const i32 dry = bus[c][n];
            line[w] = dry >> 2;
            const i32 d = BASE_Q8 + mulq(mod[c], depth);              // 2 .. 42 frames, * 256
            const u32 whole = (u32)d >> 8, frac = (u32)d & 255u;
            const i32 a0 = line[(w - whole) & LINE_MASK], a1 = line[(w - whole - 1u) & LINE_MASK];
            const i32 wob = a0 + mulq(a1 - a0, (i32)(frac << 23));
            const i32 grit = wob - line[(w - (u32)BASE) & LINE_MASK];   // both quarter scale: no overflow
            st.lp[c] = addq(st.lp[c], mulq(subq(grit, st.lp[c]), tone));
            bus[c][n] = addq(dry, shl_held(mulq(m, st.lp[c]), 2));
        }
        w = (w + 1u) & LINE_MASK;
    }
    st.wpos = w;
    st.mix = want;
    st.depth = depth_want;
}

extern "C" void veffrosion_reset(unsigned place, void* state) OP1F_NAMED("veffrosion_reset");
extern "C" void veffrosion_reset(unsigned place, void* state) {
    (void)place;
    if (state) clear(*(State*)state);
}

static volatile u32 reread;

extern "C" void veffrosion_shown(unsigned where, unsigned how) OP1F_NAMED("veffrosion_shown");
extern "C" void veffrosion_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffrosion_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffrosion_changed");
extern "C" void veffrosion_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread)
static volatile int pending_turn[2];
static volatile u32 pending_click;                                     // bit 0: knob 1 pressed, bit 3: knob 4

extern "C" int veffrosion_input(const Op1fInput* e) OP1F_NAMED("veffrosion_input");
extern "C" int veffrosion_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {
        const int steps = e->turn >> 10;
        if (steps && e->key < 2) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        if (e->what != (u32)OP1F_DOWN_SHIFT && !e->shift && (knob == 0 || knob == 3)) pending_click |= 1u << knob;
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

static void extras_start(u32 p) { for (int k = 0; k < EXTRAS; ++k) extra[p][k] = EXTRA_START[k]; }

static int read_member(const char* t, char name, int* out) {           // our own form only: {"m":1,"t":70,"v":1}
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
    char text[64];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffrosion, where, which, KEY, text, sizeof text) > 0) {
        static const char NAMES[EXTRAS] = { 'm', 't' };
        for (int k = 0; k < EXTRAS; ++k) { int v; if (read_member(text, NAMES[k], &v)) extra[p][k] = held(v, 0, EXTRA_MOST[k]); }
    }
    extra_ready |= 1u << p;
}

static u32 unsaved;

static void extras_write(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[64], * q = text;
    q = put(q, "{\"m\":"); q = put_number(q, extra[p][E_MODE]);
    q = put(q, ",\"t\":"); q = put_number(q, extra[p][E_TONE]);
    put(q, ",\"v\":1}");
    if (op1f_value_set(&op1f_card_veffrosion, where, which, KEY, text) == 0) unsaved &= ~(1u << p);
}

// ---------------------------------------------------------------- the page (display thread): veff_ui.h's common page
static char* put_hz(char* p, int hz) {                                 // 30 Hz, 440 Hz, 1.6 kHz, 12 kHz
    if (hz < 1000) return put(put_number(p, hz), " Hz");
    u32 n = (u32)hz, k = 0;
    while (n >= 1000u) { n -= 1000u; ++k; }
    if (k < 10u) {
        u32 tenths = 0; n += 50u;
        while (n >= 100u) { n -= 100u; ++tenths; }
        if (tenths >= 10u) { ++k; tenths = 0; }
        p = put_number(p, (int)k);
        if (k < 10u) { p = put(p, "."); p = put_number(p, (int)tenths); }
    } else p = put_number(p, (int)k);
    return put(p, " kHz");
}
static char* put_pct(char* p, int v) { return put(put_number(p, v), "%"); }

static void paint_picture(void* canvas, u32 where);

static void paint(void* canvas, u32 where, u32 how, int shift) {
    static char texts[4][16];
    veffui::Column col[4];
    if (!shift) {
        static const char* const LABELS[4] = { "FREQ", "WIDTH", "AMOUNT", "MIX" };
        for (int i = 0; i < 4; ++i) {
            int value = 0;
            col[i].label = LABELS[i];
            if (op1f_param_heard(&op1f_card_veffrosion, ASK_EFFECT, where, (unsigned)i, &value) == 0) {
                if (i == V_FREQ) { const int fi = held(value, 0, FREQ_STEPS - 1); put_hz(texts[i], FREQ_HZ[fi]); col[i].frac = (fi * 2064) >> 8; }
                else { const int v = held(value, 0, 100); put_pct(texts[i], v); col[i].frac = (v * 2622) >> 8; }
            } else { put(texts[i], "-"); col[i].frac = 0; }
            col[i].value = texts[i];
        }
    } else {
        const int mode = held(where < (u32)PLACES ? extra[where][E_MODE] : EXTRA_START[E_MODE], 0, 2);
        const int tone = held(where < (u32)PLACES ? extra[where][E_TONE] : EXTRA_START[E_TONE], 0, 100);
        col[0].label = "MODE"; col[0].value = MODE_NAMES[mode]; col[0].frac = mode << 9;
        put_pct(texts[1], tone);
        col[1].label = "TONE"; col[1].value = texts[1]; col[1].frac = (tone * 2622) >> 8;
        col[2].label = col[3].label = ""; col[2].value = col[3].value = ""; col[2].frac = col[3].frac = 0;
    }
    paint_picture(canvas, where);
    veffui::page(canvas, "ROSION", veffui::ICON_ROSION, col, shift ? "SHIFT" : 0, (how & (u32)OP1F_SHOWN_OFF) ? 0 : 1);
}

// ---------------------------------------------------------------- the picture (display thread): the erosion comb
// In the band VEFF_ART_TOP .. VEFF_ART_BOTTOM: a white axis and 49 vertical pink lines, 9 px apart, with white dots at
// their tips. A bell around FREQ's place (log axis) of WIDTH's breadth, AMOUNT tall, with two side lobes (the wobble's
// sidebands; far apart in wide mode) makes the tallest lines; the rest fade to small dots on the axis. The grain moves
// with the frame counter (a sine in sine mode; jagged and from the input's snapshot in noise and wide), its size with
// the input's level. MIX brightens the pink. Integer math only; about 200 primitives a frame.
static const unsigned char EXP_DOWN[64] = {   // 255 exp(-i/16)
    255,240,225,211,199,187,175,165,155,145,136,128,120,113,106,100,94,88,83,78,73,69,64,61,57,53,50,47,44,42,39,37,
    35,32,30,29,27,25,24,22,21,20,18,17,16,15,14,14,13,12,11,11,10,9,9,8,8,7,7,6,6,6,5,5 };
enum { LINES = 49, LINE_DX = 9, LINE_LEFT = 24 };   // 24 .. 456: the common axis

static inline int sin8(int a) { return SINE[a & 1023] >> 23; }       // a: 1024 a turn; out +-255
static u32 divide(u32 n, u32 d) {                                      // no library call
    u32 q = 0, r = 0;
    for (int b = 31; b >= 0; --b) { r = (r << 1) | ((n >> b) & 1u); if (r >= d) { r -= d; q |= 1u << b; } }
    return q;
}
static inline int bell(int d, u32 inv) {                               // d px from the centre: 0 .. 255
    const int r = (int)(((u32)d * inv) >> 8), x2 = (r * r) >> 8;
    return (x2 >> 4) < 64 ? EXP_DOWN[x2 >> 4] : 0;
}

static void paint_picture(void* canvas, u32 where) {
    using namespace veffui;
    static u32 tick;
    static int env, wave[SNAP];
    ++tick;
    if (snap_fill >= (u32)SNAP) {                                      // a whole snapshot: take it, ask for the next
        for (int i = 0; i < SNAP; ++i) wave[i] = snap[i] >> 23;        // +-256
        snap_fill = 0;
    }
    const int level = (int)(peak_word >> 23);                          // 0 .. 256
    env += (level - env) >> 2;
    int v[4] = { FREQ_START, 30, 40, 50 };
    for (int i = 0; i < 4; ++i) { int x; if (op1f_param_heard(&op1f_card_veffrosion, ASK_EFFECT, where, (unsigned)i, &x) == 0) v[i] = x; }
    const int fi = held(v[V_FREQ], 0, FREQ_STEPS - 1), wp = held(v[V_WIDTH], 0, 100);
    const int ap = held(v[V_AMOUNT], 0, 100), mp = held(v[V_MIX], 0, 100);
    const int mode = held(where < (u32)PLACES ? extra[where][E_MODE] : EXTRA_START[E_MODE], 0, 2);
    const int tone = held(where < (u32)PLACES ? extra[where][E_TONE] : EXTRA_START[E_TONE], 0, 100);
    const int t = (int)tick;
    const int cx = 48 + ((fi * 384) >> 7);                             // FREQ's place: x 48 .. 432
    const u32 inv = divide(65536u, (u32)(12 + ((wp * 160) >> 8))); // 256 / breadth (12 .. 74 px)
    const int lobe = (mode == MODE_WIDE ? 120 : 84) + ((wp * 40) >> 8); // the sidebands' distance
    const int amount = 110 + ((ap * 360) >> 8);                         // ~0.4 .. 0.9 of the band's half
    const int grow = 200 + ((env * 100) >> 8) + ((tone * 30) >> 8);   // the level and TONE grow it
    const int k = 90 + ((mp * 425) >> 8);                              // MIX: 0.35 .. 1
    const int half = VEFF_ART_HALF - 1;                                 // 49 px, the dot's row within 50
    axis(canvas);
    for (int j = 0; j < LINES; ++j) {
        const int x = LINE_LEFT + j * LINE_DX;
        const int d = x > cx ? x - cx : cx - x;
        const int dl = d > lobe ? d - lobe : lobe - d;
        int h = bell(d, inv) + ((bell(dl, inv) * 150) >> 8);           // 0 .. ~400
        h = h > 255 ? 255 : h;
        int grain;
        if (mode == MODE_SINE) grain = sin8(d * 9 - t * 12);
        else grain = (sin8(d * 37 + t * 22) * sin8(d * 11 - t * 9 + 256) >> 8) + (wave[((u32)(j + t) * 21u >> 3) & 31u] >> 1);
        grain = held(grain, -255, 255);
        const int mag = held(((h * amount) >> 8) * (grow + ((grain * env) >> 8)) >> 8, 0, 256);
        const int up = (mag * half) >> 8;
        const int down = (mag * (half - 8 + ((grain * 10) >> 8))) >> 8;
        const unsigned c = pink((k * (150 + (mag >> 1))) >> 8);
        if (up < 3) { op1f_paint_box(canvas, x - 1, VEFF_AXIS_Y - 1, 3, 3, pink(k)); continue; }
        op1f_paint_line(canvas, x, VEFF_AXIS_Y - up, x, VEFF_AXIS_Y + down, 1, c);
        const unsigned dot = mag > 64 ? VEFF_WHITE : white(120 + mag * 2);
        op1f_paint_box(canvas, x - 1, VEFF_AXIS_Y - up - 1, 3, 3, dot);
        op1f_paint_box(canvas, x - 1, VEFF_AXIS_Y + down - 1, 3, 3, dot);
    }
    sparkles(canvas);
}

static u32 asked;

extern "C" void veffrosion_frame(const Op1fFrame* now) OP1F_NAMED("veffrosion_frame");
extern "C" void veffrosion_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffrosion, "veffrosionpage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffrosion, ASK_EFFECT, page_doc);
    }
    if (!asked) { asked = 1; for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p); }
    const u32 riding = (u32)op1f_riding(&op1f_card_veffrosion, ASK_EFFECT);
    const u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        static const int STEP[EXTRAS] = { 1, 2 };
        for (int k = 0; k < EXTRAS; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            extra[where][k] = held(extra[where][k] + t * STEP[k], 0, EXTRA_MOST[k]);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 1u) op1f_param_set(&op1f_card_veffrosion, ASK_EFFECT, where, V_FREQ, FREQ_START);
        if (clicks & 8u) op1f_param_set(&op1f_card_veffrosion, ASK_EFFECT, where, V_MIX, 0);
    } else {
        pending_turn[0] = pending_turn[1] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffrosion && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP))
        paint(now->canvas, where, how, now->shift != 0);
}
