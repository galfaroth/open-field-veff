// veffshaper: a parametric waveshaper, as an effect of the unit's own lists (level 8 of open-field's base, with
// level 7 for the values past the four knobs and level 10 for the page). Our own implementation in the spirit of a
// waveshaper mode. It changes no place of the firmware and names no address of it.
//
// What the owner meets:
//   `veffshaper` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer), and on
//   every wagon of fx train. It stands on the unit's `punch`.
//   The four knobs: DRIVE (-12 .. +24 dB, 0 dB at 50), CURVE (the saturating cubic), DEPTH (the sine ripple), MIX.
//   With shift held, the four knobs turn the values kept with the sound (level 7, `veffshaper_set`):
//   LIN (the linear part), DAMP (flattens near zero), PERIOD (the ripple's cycles, 0.5 .. 20), OUTPUT (0 .. -24 dB).
//   A press of a knob: drive to 0 dB, curve to 0, depth to 0, mix to 100. With shift: that extra to its start.
//
// The shape, for u = drive x in, held to full scale:
//   w   = smoothstep(|u| / t)     damp's gate, t = damp x 0.25 of full scale (w = 1 at damp 0)
//   v   = u w
//   y   = lin v + curve (1.5 v - 0.5 v^3) + depth/2 sin(2 pi period v)     held to full scale
//   out = y x output, then dry (1 - mix) + out mix
// Odd in u: silence in, silence out, no DC. It keeps nothing but the last block's coefficients, which it glides to
// the new ones across each block: the shape is computed at run time (no table to swap), so knob sweeps and LFOs are
// smooth. On the audio thread: no division, no 64-bit multiply, no float; every sum held at full scale.

#include "build_r6e_card.h"
#include "veff_ui.h"
#include "veffshaper_tables.h"

typedef unsigned int u32;
typedef int i32;

enum { ASK_EFFECT = 0, ASK_KEEPS = 1, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { CHANNELS = 2, BLOCK = 64 };
enum { V_DRIVE = 0, V_CURVE = 1, V_DEPTH = 2, V_MIX = 3 };
enum { E_LIN = 0, E_DAMP = 1, E_PERIOD = 2, E_OUTPUT = 3, EXTRAS = 4 };
enum { PLACES = 17 };
static const int EXTRA_START[EXTRAS] = { 50, 0, 20, 0 };              // output: attenuation in dB, 0 .. 24
static const int EXTRA_MOST[EXTRAS] = { 100, 100, 100, 24 };
static const char KEY[] = "veffshaper_set";
static const char LOG_FILE[] = "op1f/veffshaper.cpp";

extern "C" const Op1fCard op1f_card_veffshaper OP1F_NAMED("op1f_card_veffshaper");

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

// ---------------------------------------------------------------- the values past the four knobs
static volatile int extra[PLACES][EXTRAS];
static volatile u32 extra_ready;

static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- the shape (shared by the sound and the page)
struct Coef { i32 drive, lin, curve, depth, gate, period, out; };    // drive Q27; gate GATE_R; period cycles x 2048

static void coef_of(Coef* c, int drive, int curve, int depth, int lin, int damp, int period, int output) {
    c->drive = DRIVE_GAIN[held(drive, 0, 100)];
    c->curve = held(curve, 0, 100) >= 100 ? 0x7fffffff : held(curve, 0, 100) * 21474836;
    c->depth = held(depth, 0, 100) * 5368709;                          // 0 .. 0.25: depth/2 in the half scale
    c->lin = held(lin, 0, 100) >= 100 ? 0x7fffffff : held(lin, 0, 100) * 21474836;
    c->gate = GATE_R[held(damp, 0, 100)];
    c->period = 1024 + held(period, 0, 100) * 399;                     // 0.5 .. 20 cycles over the full scale
    c->out = OUT_GAIN[held(output, 0, 24)];
}

static inline i32 sine(u32 phase) {
    const u32 i = phase >> 22;
    const i32 f = (i32)((phase >> 6) & 0xffffu) << 15;
    const i32 s0 = SINE[i], s1 = SINE[i + 1];
    return s0 + mulq((s1 >> 1) - (s0 >> 1), f) * 2;
}

static inline i32 abs_held(i32 x) { return x < 0 ? (x == (i32)0x80000000 ? 0x7fffffff : -x) : x; }

// f(u): u in Q31 (already driven and held), answers y in Q31, held
static inline i32 shape(i32 u, i32 lin, i32 curve, i32 depth, i32 gate, i32 period) {
    if (gate) {                                                        // damp: a smooth gate near zero
        const i32 a = mulq(abs_held(u), gate);
        i32 w = a >= (1 << 22) ? 0x7fffffff : a << 9;
        const i32 w2 = mulq(w, w);
        const i32 s = mulq(w2, 0x60000000 - (w >> 1));                 // w^2 (3 - 2w) / 4
        w = s >= 0x20000000 ? 0x7fffffff : s << 2;
        u = mulq(u, w);
    }
    const i32 u3 = mulq(mulq(u, u), u);
    const i32 soft_half = mulq(u, 0x60000000) - (u3 >> 2);             // (1.5 u - 0.5 u^3) / 2
    const u32 ph = (u32)(u >> 10) * (u32)period;                       // u x period turns, wrapping as a phase does
    i32 y = addq(mulq(lin, u) >> 1, mulq(curve, soft_half));
    y = addq(y, mulq(depth, sine(ph)));                                // all in the half scale
    return y >= 0x40000000 ? 0x7fffffff : y < -0x40000000 ? (i32)0x80000000 : y * 2;
}

// ---------------------------------------------------------------- the sound (audio thread)
struct State { Coef c; i32 mix; u32 ready; };
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 64, "the recipe's state_bytes");

static volatile u32 level;
// the page's feed: the dry left bus, one frame in 16, into a ring of 48 (plain word stores; torn reads are fine)
enum { WAVE = 48, WAVE_STEP = 16 };
static volatile i32 wave[WAVE];
static volatile u32 wave_at;
static volatile u32 shown_where, shown_how;

extern "C" void veffshaper_process(const Op1fEffectBlock* now) OP1F_NAMED("veffshaper_process");
extern "C" void veffshaper_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    if (now->fresh) { st.ready = 0; st.mix = 0; }
    {
        const u32 sw = shown_where, pl = now->place;
        if (pl == sw || (pl >= 32u && sw >= 32u)) {
            const u32 fr = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
            const int* in = now->left;
            u32 w = wave_at;
            if (w >= (u32)WAVE) w = 0;
            for (u32 n = 0; n < fr; n += WAVE_STEP) { wave[w] = in[n]; if (++w >= (u32)WAVE) w = 0; }
            wave_at = w;
        }
    }
    const int* v = now->values;
    const volatile int* e = extras_of(now->place);
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = e ? e[k] : EXTRA_START[k];
    Coef to;
    coef_of(&to, v[V_DRIVE], v[V_CURVE], v[V_DEPTH], x[E_LIN], x[E_DAMP], x[E_PERIOD], x[E_OUTPUT]);
    const i32 want = held(v[V_MIX], 0, 100) >= 100 ? 0x7fffffff : held(v[V_MIX], 0, 100) * 21474836;
    if (!st.ready) { st.c = to; st.ready = 1; }
    const i32 from = st.mix;
    if (!now->on || (want == 0 && from == 0)) { st.c = to; return; }   // off, or mix 0: the sound as it came
    Coef c = st.c;
    c.gate = to.gate;                                                  // damp steps on the shift page: taken at once (a glide through 0 would gate everything)
    // glide every coefficient across the block (64 frames): steps in 64ths, the last frame lands on the target
    const i32 sd = (to.drive >> 6) - (c.drive >> 6), sl = (to.lin >> 6) - (c.lin >> 6), sc = (to.curve >> 6) - (c.curve >> 6);
    const i32 sp = (to.depth >> 6) - (c.depth >> 6), so = (to.out >> 6) - (c.out >> 6);
    const i32 sm = (want >> 6) - (from >> 6);
    const i32 per0 = c.period, dper = to.period - c.period;
    int* bus[CHANNELS] = { now->left, now->right };
    const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    i32 m = from, peak = 0;
    for (u32 n = 0; n < frames; ++n) {
        c.drive += sd; c.lin += sl; c.curve += sc; c.depth += sp; c.out += so; m += sm;
        const i32 period = per0 + ((dper * (i32)(n + 1)) >> 6);
        for (int ch = 0; ch < CHANNELS; ++ch) {
            const i32 dry = bus[ch][n];
            i32 u = mulq(dry, c.drive);                                // x gain / 16
            u = u >= (1 << 27) ? 0x7fffffff : u < -(1 << 27) ? (i32)0x80000000 : u << 4;
            const i32 o = mulq(shape(u, c.lin, c.curve, c.depth, c.gate, period), c.out);
            const i32 mag = abs_held(o);
            if (mag > peak) peak = mag;
            bus[ch][n] = addq(mulq(0x7fffffff - m, dry), mulq(m, o));
        }
    }
    st.c = to;
    st.mix = want;
    level = (u32)peak;
}

extern "C" void veffshaper_reset(unsigned place, void* state) OP1F_NAMED("veffshaper_reset");
extern "C" void veffshaper_reset(unsigned place, void* state) {
    (void)place;
    if (state == 0) return;
    u32* w = (u32*)state;
    for (u32 i = 0; i < (u32)(STATE_BYTES >> 2); ++i) w[i] = 0;
}

// ---------------------------------------------------------------- told by the base (display thread)
static volatile u32 reread;

extern "C" void veffshaper_shown(unsigned where, unsigned how) OP1F_NAMED("veffshaper_shown");
extern "C" void veffshaper_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffshaper_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffshaper_changed");
extern "C" void veffshaper_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread)
static volatile int pending_turn[4];
static volatile u32 pending_click;                                     // bits 0..3: a press; bits 4..7: shift and a press

extern "C" int veffshaper_input(const Op1fInput* e) OP1F_NAMED("veffshaper_input");
extern "C" int veffshaper_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {
        const int steps = e->turn >> 10;
        if (steps) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        pending_click |= (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) ? 16u << knob : 1u << knob;
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
    do { u32 tens = 0; while (n >= 10u) { n -= 10u; ++tens; __asm__("" : "+r"(n)); } d[k++] = (char)('0' + n); n = tens; } while (n && k < 11);
    while (k) *p++ = d[--k];
    *p = 0; return p;
}

static void extras_start(u32 p) { for (int k = 0; k < EXTRAS; ++k) extra[p][k] = EXTRA_START[k]; }

// Our own form only: {"d":0,"l":50,"o":0,"p":20,"v":1}
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

static const char NAMES[EXTRAS] = { 'l', 'd', 'p', 'o' };

static void extras_read(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffshaper, where, which, KEY, text, sizeof text) > 0)
        for (int k = 0; k < EXTRAS; ++k) { int v; if (read_member(text, NAMES[k], &v)) extra[p][k] = held(v, 0, EXTRA_MOST[k]); }
    extra_ready |= 1u << p;
}

static u32 unsaved;

static void extras_write(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96], * q = text;
    q = put(q, "{\"d\":"); q = put_number(q, extra[p][E_DAMP]);
    q = put(q, ",\"l\":"); q = put_number(q, extra[p][E_LIN]);
    q = put(q, ",\"o\":"); q = put_number(q, extra[p][E_OUTPUT]);
    q = put(q, ",\"p\":"); q = put_number(q, extra[p][E_PERIOD]);
    put(q, ",\"v\":1}");
    if (op1f_value_set(&op1f_card_veffshaper, where, which, KEY, text) == 0) unsaved &= ~(1u << p);
}

// ---- the page: the common Veff page (veff_ui.h), with the shaped wave as the picture: the input sine (pink edge),
// the wave as the sound shapes it (white, on top), the pink area between them, ring markers riding the curve
enum { PLOT_LEFT = 24, PLOT_RIGHT = 456, PLOT_HALF = 40 };              // full scale: 40 px either side (+ the rings: 47)

static inline int plot_y(i32 y) { return veffui::VEFF_AXIS_Y - (((y >> 16) * PLOT_HALF) >> 15); }
static inline int plot_x(int i) { return PLOT_LEFT + ((i * 589) >> 6); }   // 48 points over 24 .. 456, centred

static inline i32 shaped(i32 dry, const Coef& c) {                     // as the sound does it, before output
    i32 u = mulq(dry, c.drive);
    u = u >= (1 << 27) ? 0x7fffffff : u < -(1 << 27) ? (i32)0x80000000 : u << 4;
    return shape(u, c.lin, c.curve, c.depth, c.gate, c.period);
}
static const char* const KNOB_NAMES[4] = { "DRIVE", "CURVE", "DEPTH", "MIX" };
static const char* const EXTRA_NAMES[4] = { "LIN", "DAMP", "PERIOD", "OUTPUT" };

static char* put_db(char* q, int db) {                                 // +12 dB, -6 dB, 0 dB
    if (db > 0) q = put(q, "+");
    q = put_number(q, db);
    return put(q, " dB");
}
static char* put_hundredths(char* q, int v) {                          // 0..100 as 0.00 .. 1.00
    if (v >= 100) return put(q, "1.00");
    q = put(q, "0.");
    int tens = 0; while (v >= 10) { v -= 10; ++tens; __asm__("" : "+r"(v)); }
    *q++ = (char)('0' + tens); *q++ = (char)('0' + v); *q = 0;
    return q;
}
static inline int frac100(int v) { return (held(v, 0, 100) * 10486) >> 10; }   // 0..100 to 0..1024

static void paint(void* canvas, u32 where, u32 how, int shift) {
    int v[4];
    for (u32 i = 0; i < 4u; ++i) if (op1f_param_heard(&op1f_card_veffshaper, ASK_EFFECT, where, i, &v[i]) != 0) v[i] = i == 3 ? 100 : 0;
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = where < (u32)PLACES ? extra[where][k] : EXTRA_START[k];
    static char texts[4][16];
    veffui::Column col[4];
    for (int c = 0; c < 4; ++c) {
        char* q = texts[c];
        int fr;
        if (!shift) {
            if (c == V_DRIVE) put_db(q, DRIVE_DB[held(v[0], 0, 100)]);
            else if (c == V_CURVE) put_hundredths(q, held(v[c], 0, 100));
            else { q = put_number(q, held(v[c], 0, 100)); put(q, "%"); }
            fr = frac100(v[c]);
        } else if (c == E_PERIOD) {
            const int t = PERIOD_X10[held(x[c], 0, 100)];
            int whole = 0, r = t; while (r >= 10) { r -= 10; ++whole; __asm__("" : "+r"(r)); }
            q = put_number(q, whole); q = put(q, "."); put_number(q, r);
            fr = frac100(x[c]);
        } else if (c == E_OUTPUT) {
            put_db(q, -held(x[c], 0, 24));
            fr = 1024 - ((held(x[c], 0, 24) * 43691) >> 10);           // 0 dB at the right
        } else { q = put_number(q, held(x[c], 0, 100)); put(q, "%"); fr = frac100(x[c]); }
        col[c].label = shift ? EXTRA_NAMES[c] : KNOB_NAMES[c];
        col[c].value = texts[c];
        col[c].frac = fr;
    }
    const int on = (how & (u32)OP1F_SHOWN_OFF) ? 0 : 1;
    veffui::page(canvas, "SHAPER", veffui::ICON_SHAPER, col, shift ? "SHIFT" : 0, on);

    Coef cf;
    coef_of(&cf, v[V_DRIVE], v[V_CURVE], v[V_DEPTH], x[E_LIN], x[E_DAMP], x[E_PERIOD], x[E_OUTPUT]);
    const i32 m = held(v[V_MIX], 0, 100) >= 100 ? 0x7fffffff : held(v[V_MIX], 0, 100) * 21474836;
    static u32 tick;
    static i32 env;                                                    // Q31, follows the wave's peak
    ++tick;
    i32 in[WAVE];
    const u32 at = wave_at;
    i32 peak = 0;
    for (u32 i = 0, k = at; i < (u32)WAVE; ++i) {
        if (k >= (u32)WAVE) k = 0;
        in[i] = wave[k++];
        const i32 a = abs_held(in[i]);
        if (a > peak) peak = a;
    }
    env += (peak >> 4) - (env >> 4);                                   // slow, so the picture breathes, not flickers
    {   // a clean sine, one and a half turns, whose height follows the sound (the raw snapshot aliases and jumps)
        const i32 amp = 0x30000000 + (env >> 1) + (sine(tick * 15000000u) >> 4);   // ~0.33 .. 0.94
        for (u32 i = 0; i < (u32)WAVE; ++i) in[i] = mulq(sine(i * 134217728u - tick * 22800000u), amp);
    }
    const unsigned ink = on ? VEFF_PINK : VEFF_GREY, white = on ? VEFF_WHITE : VEFF_GREY;
    const int glow = 150 + (int)(((env >> 16) * 90) >> 15);            // the fill's strength: 150 .. 240 of 256
    const unsigned fill = on ? veffui::pink(glow) : veffui::dim(0x85, 0x83, 0x8b, 80);
    int yi[WAVE], yo[WAVE];
    for (int i = 0; i < WAVE; ++i) {                                   // the shaped wave, as the sound makes it
        const i32 dry = in[i];
        const i32 o = mulq(shaped(dry, cf), cf.out);
        yo[i] = plot_y(addq(mulq(0x7fffffff - m, dry), mulq(m, o)));
        yi[i] = plot_y(dry);
        if (i) {                                                       // the pink area between in and out
            const int x0 = plot_x(i - 1), x1 = plot_x(i);
            const int a = (yi[i - 1] + yi[i]) >> 1, b = (yo[i - 1] + yo[i]) >> 1;
            const int top = a < b ? a : b, h = a < b ? b - a : a - b;
            if (h) op1f_paint_box(canvas, x0, top, x1 - x0 + 1, h, fill);
        }
    }
    veffui::axis(canvas, white);
    veffui::dashed_v(canvas, veffui::VEFF_MID_X, veffui::VEFF_AXIS_Y - veffui::VEFF_ART_HALF, veffui::VEFF_AXIS_Y + veffui::VEFF_ART_HALF, white);
    for (int i = 1; i < WAVE; ++i) op1f_paint_line(canvas, plot_x(i - 1), yi[i - 1], plot_x(i), yi[i], 1, ink);
    for (int i = 1; i < WAVE; ++i) op1f_paint_line(canvas, plot_x(i - 1), yo[i - 1], plot_x(i), yo[i], 2, white);
    const int r = 5 + (int)(((env >> 16) * 2) >> 15);
    for (int j = 0; j < 5; ++j) {                                      // rings at .1 .3 .5 .7 .9, swaying
        const i32 frac = 6554 + j * 13107 + (mulq(sine(tick * 17100000u + (u32)j * 683565275u), 37580963) >> 15);   // Q16
        const int i = held((frac * (WAVE - 1)) >> 16, 0, WAVE - 1);
        const int X = plot_x(i), Y = yo[i];
        veffui::disc(canvas, X, Y, r, OP1F_COLOUR(0, 0, 0));
        veffui::ring(canvas, X, Y, r, r, white, 2, 3);
    }
    veffui::sparkles(canvas);
}

static struct { u32 asked; } f;

extern "C" void veffshaper_frame(const Op1fFrame* now) OP1F_NAMED("veffshaper_frame");
extern "C" void veffshaper_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffshaper, "veffshaperpage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffshaper, ASK_EFFECT, page_doc);
    }
    if (!f.asked) {
        f.asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
        op1f_log(3, LOG_FILE, 1, "veffshaper: ready");
    }
    const u32 riding = (u32)op1f_riding(&op1f_card_veffshaper, ASK_EFFECT);
    u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        for (int k = 0; k < 4; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            extra[where][k] = held(extra[where][k] + t, 0, EXTRA_MOST[k]);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        static const int PRESSED[4] = { 50, 0, 0, 100 };               // drive 0 db, curve 0, depth 0, mix 100
        for (int k = 0; k < 4; ++k) {
            if (clicks >> k & 1u) op1f_param_set(&op1f_card_veffshaper, ASK_EFFECT, where, (unsigned)k, PRESSED[k]);
            if (clicks >> (4 + k) & 1u) { extra[where][k] = EXTRA_START[k]; unsaved |= 1u << where; }
        }
    } else {
        for (int k = 0; k < 4; ++k) pending_turn[k] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffshaper && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP))
        paint(now->canvas, where, how, now->shift != 0);
}
