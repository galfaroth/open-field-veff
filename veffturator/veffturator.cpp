// veffturator: a saturator after Ableton's Saturator (our own implementation), as an effect of the unit's own lists
// (level 8 of open-field's base, with level 7 for the values past the four knobs and level 10 for the page). It changes
// no place of the firmware and names no address of it.
//
// What the owner meets:
//   `veffturator` in the unit's lists of effects (shift and T3 on a sound, the master's list on the mixer), and on
//   every wagon of fx train. It stands on the unit's `punch`: a sound saved with it keeps punch, switched off, elsewhere.
//   The four knobs (and the LFOs): DRIVE (-36 .. +36 dB), TYPE (analog, sine, medium, hard, fold, digital),
//   OUTPUT (-36 .. 0 dB), MIX (0 .. 100). A press of the 1st knob puts the drive to 0 dB, of the 3rd the output.
//   With shift held, the knobs turn the values kept with the sound (level 7, `veffturator_set`):
//   COLOR (amount -100 .. +100 of a peak before the shaper, -12 .. +12 dB; turning it switches it on; shift and a press
//   of the 1st knob switches it off and on), FREQ (the colour's 100 Hz .. 10 kHz), CLIP (a soft clip on the output:
//   right on, left off; shift and a press toggles), DC (the DC filter: the same).
//
// Per side: drive -> colour (a TPT state-variable band-pass added to the sound) -> 2x up (half-band FIR) -> shaper
// (1025-point tables, interpolated) -> 2x down -> DC filter -> output -> soft clip -> mix (the dry delayed to match).
// On the audio thread: no division, no 64-bit multiply, no float; every sum and difference held at full scale.

#include "build_r6e_card.h"
#include "veff_ui.h"
#include "veffturator_tables.h"

typedef unsigned int u32;
typedef int i32;

enum { ASK_EFFECT = 0, ASK_KEEPS = 1, BLOCK_LEVEL_8 = 44, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { CHANNELS = 2, BLOCK = 64, XH = 9, WH = 18, DELAY = 9 };   // the up and down filters' histories; their delay
enum { V_DRIVE = 0, V_TYPE = 1, V_OUTPUT = 2, V_MIX = 3 };
enum { E_AMOUNT = 0, E_FREQ = 1, E_SOFT = 2, E_DC = 3, E_COLOR = 4, EXTRAS = 5 };
enum { PLACES = 17 };                                                  // the sounds' slots 0..15 and the master
static const int EXTRA_START[EXTRAS] = { 30, 30, 0, 1, 0 };            // colour +30 at 1 kHz (off), no clip, dc on
static const int EXTRA_LEAST[EXTRAS] = { -100, 0, 0, 0, 0 };
static const int EXTRA_MOST[EXTRAS] = { 100, FREQ_STEPS - 1, 1, 1, 1 };
static const char KEY[] = "veffturator_set";
static const char LOG_FILE[] = "op1f/veffturator.cpp";
static const char* const TYPE_NAMES[CURVES] = { "analog", "sine", "medium", "hard", "fold", "digital" };

extern "C" const Op1fCard op1f_card_veffturator OP1F_NAMED("op1f_card_veffturator");

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
static volatile u32 extra_ready;                                       // bit n: place n's values were read

static inline const volatile int* extras_of(u32 place) {
    return place < (u32)PLACES && (extra_ready >> place & 1u) ? extra[place] : 0;
}

// ---------------------------------------------------------------- told by the base (display thread): single words
static volatile u32 shown_where, shown_how;

// ---------------------------------------------------------------- the page's feed (audio thread writes, frame reads)
// Plain aligned words; a torn read only moves a line on the page for one frame.
enum { WAVE = 48 };
static volatile i32 feed_in[WAVE], feed_out[WAVE];                     // two samples a call, in and out, left side
static volatile u32 feed_peak, feed_at;                                // |in| peak of the last call; the next slot

static inline void feed_end(u32 at, u32 half, const int* l) {
    feed_out[at] = l[0]; feed_out[at + 1] = l[half];
    feed_at = at + 2u >= (u32)WAVE ? 0u : at + 2u;
}

// ---------------------------------------------------------------- the sound (audio thread)
struct Channel {
    i32 ic1, ic2, dc;                                                  // the colour's two integrators, the DC filter's low-pass
    i32 x[XH + BLOCK];                                                 // driven, Q22, with the up filter's history
    i32 w[WH + 2 * BLOCK];                                             // shaped at 2x, Q31, with the down filter's history
    i32 dry[DELAY + BLOCK];                                            // the dry, delayed as the wet is
};
struct State { Channel ch[CHANNELS]; i32 drive, out, mix; u32 primed; };
enum { STATE_BYTES = (sizeof(State) + 31) & ~31 };
static_assert(STATE_BYTES == 2400, "the recipe's state_bytes");

static inline i32 shape(const int* t, i32 v) {                         // v in Q22 (|u| up to 8 in the table), out Q31
    const u32 neg = (u32)v >> 31;
    u32 m = neg ? (v == (i32)0x80000000 ? 0x7fffffffu : (u32)(0 - v)) : (u32)v;
    i32 y;
    if (m >= (1u << 25)) y = t[CURVE_POINTS - 1];
    else {
        const u32 i = m >> 15;
        const i32 f = (i32)((m & 0x7fffu) << 16);
        const i32 t0 = t[i], t1 = t[i + 1];
        y = t0 + mulq((t1 >> 1) - (t0 >> 1), f) * 2;
    }
    return neg ? subq(0, y) : y;                                       // table values never reach -2^31
}

static inline i32 twice(i32 x) { return addq(x, x); }

extern "C" void veffturator_process(const Op1fEffectBlock* now) OP1F_NAMED("veffturator_process");
extern "C" void veffturator_process(const Op1fEffectBlock* now) {
    if (now->bytes < (u32)BLOCK_LEVEL_8 || now->state == 0) return;
    State& st = *(State*)now->state;
    const int* v = now->values;
    const volatile int* e = extras_of(now->place);
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = held(e ? e[k] : EXTRA_START[k], EXTRA_LEAST[k], EXTRA_MOST[k]);
    const i32 drive_to = DRIVE_GAIN[held(v[V_DRIVE], -36, 36) + 36];
    const int* table = CURVE[held(v[V_TYPE], 0, CURVES - 1)];
    const i32 out_to = OUT_GAIN[held(v[V_OUTPUT], -36, 0) + 36];
    const int mp = held(v[V_MIX], 0, 100);
    const i32 want = mp >= 100 ? 0x7fffffff : mp * 21474836;
    if (now->fresh || !st.primed) {                                    // forget the filters; the gains start where asked
        u32* w = (u32*)now->state;
        for (u32 i = 0; i < (u32)(STATE_BYTES >> 2); ++i) w[i] = 0;
        st.drive = drive_to; st.out = out_to; st.primed = 1;
    }
    const i32 from = st.mix;
    const u32 fr = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    const u32 seen = fr != 0 && now->place == shown_where;             // feed the page only from the place it shows
    const u32 at = feed_at < (u32)WAVE - 1u ? feed_at & ~1u : 0u, half = fr >> 1;
    if (seen) {
        const int* l = now->left;
        feed_in[at] = l[0]; feed_in[at + 1] = l[half];
        u32 pk = 0;
        for (u32 k = 0; k < fr; k += 8u) { const i32 s = l[k]; const u32 m = s < 0 ? 0u - (u32)s : (u32)s; if (m > pk) pk = m; }
        feed_peak = pk;
    }
    if (!now->on || (want == 0 && from == 0)) { if (seen) feed_end(at, half, now->left); return; }   // off, or mix 0: the sound as it came
    const int color = x[E_COLOR] && x[E_AMOUNT] != 0;
    const i32 amount = x[E_AMOUNT] > 0 ? x[E_AMOUNT] * 16106127 : x[E_AMOUNT] * 4026532;   // gain / 4: +3 .. -0.75
    const int* svf = COLOR_SVF[x[E_FREQ]];
    const i32 a1 = svf[0], a2 = svf[1], a3 = svf[2];
    const int soft = x[E_SOFT], dc = x[E_DC];
    const u32 frames = now->frames > (u32)BLOCK ? (u32)BLOCK : now->frames;
    const i32 dstep = (drive_to >> 6) - (st.drive >> 6), ostep = (out_to >> 6) - (st.out >> 6), mstep = (want >> 6) - (from >> 6);
    int* bus[CHANNELS] = { now->left, now->right };
    for (int c = 0; c < CHANNELS; ++c) {
        Channel& ch = st.ch[c];
        int* b = bus[c];
        i32 g = st.drive;
        for (u32 n = 0; n < frames; ++n) {                             // drive and colour
            const i32 in = b[n];
            ch.dry[DELAY + n] = in;
            i32 s = in >> 3;                                           // Q28: room for the colour's boost
            if (color) {
                const i32 v3 = subq(s, ch.ic2);
                const i32 v1 = addq(mulq(a1, ch.ic1), mulq(a2, v3));
                const i32 v2 = addq(ch.ic2, addq(mulq(a2, ch.ic1), mulq(a3, v3)));
                ch.ic1 = subq(twice(v1), ch.ic1);
                ch.ic2 = subq(twice(v2), ch.ic2);
                s = addq(s, twice(twice(mulq(amount, v1))));
            }
            if (n < (u32)BLOCK - 1u) g += dstep;
            ch.x[XH + n] = mulq(s, g);                                 // Q28 * gain / 64: Q22
        }
        const i32* xs = ch.x + XH;
        for (u32 n = 0; n < frames; ++n) {                             // 2x up, shape
            const i32* p = xs + n;
            i32 acc = 0;
            for (int k = 0; k < HB_TAPS; ++k) acc = addq(acc, mulq(addq(p[-k], p[-9 + k]), HB[k]));
            ch.w[WH + 2 * n] = shape(table, twice(acc));
            ch.w[WH + 2 * n + 1] = shape(table, p[-4]);
        }
        i32 o = st.out, m = from;
        for (u32 n = 0; n < frames; ++n) {                             // 2x down, dc, output, clip, mix
            const i32* p = ch.w + WH + 2 * n;
            i32 acc = p[-9] >> 1;
            for (int k = 0; k < HB_TAPS; ++k) acc = addq(acc, twice(mulq((p[-2 * k] >> 1) + (p[-18 + 2 * k] >> 1), HB[k])));
            if (dc) { ch.dc = addq(ch.dc, mulq(subq(acc, ch.dc), 3060000)); acc = subq(acc, ch.dc); }   // ~10 Hz
            if (n < (u32)BLOCK - 1u) o += ostep;
            acc = mulq(acc, o);
            if (soft) acc = addq(acc, (acc >> 1) - (mulq(mulq(acc, acc), acc) >> 1));   // 1.5x - 0.5x^3: 1 at 1
            m += mstep;
            b[n] = addq(mulq(0x7fffffff - m, ch.dry[n]), mulq(m, acc));
        }
        for (int k = 0; k < XH; ++k) ch.x[k] = ch.x[frames + k];
        for (int k = 0; k < WH; ++k) ch.w[k] = ch.w[2 * frames + k];
        for (int k = 0; k < DELAY; ++k) ch.dry[k] = ch.dry[frames + k];
    }
    st.drive = drive_to; st.out = out_to; st.mix = want;
    if (seen) feed_end(at, half, now->left);
}

extern "C" void veffturator_reset(unsigned place, void* state) OP1F_NAMED("veffturator_reset");
extern "C" void veffturator_reset(unsigned place, void* state) {
    (void)place;
    if (state == 0) return;
    u32* w = (u32*)state;
    for (u32 i = 0; i < (u32)(STATE_BYTES >> 2); ++i) w[i] = 0;
}

// ---------------------------------------------------------------- told by the base (display thread): single words
static volatile u32 reread;                                            // bit n: read place n's kept values again

extern "C" void veffturator_shown(unsigned where, unsigned how) OP1F_NAMED("veffturator_shown");
extern "C" void veffturator_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

static u32 place_of(unsigned where, unsigned which) {
    if (where == (unsigned)OP1F_KEEP_SYNTH) return which & 7u;
    if (where == (unsigned)OP1F_KEEP_DRUM) return 8u + (which & 7u);
    if (where == (unsigned)OP1F_KEEP_MASTER) return 16u;
    return 32u;
}

extern "C" void veffturator_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffturator_changed");
extern "C" void veffturator_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    const u32 p = place_of(where, which);
    if (p < (u32)PLACES) reread |= 1u << p;
}

// ---------------------------------------------------------------- the keys (input thread): single words
static volatile int pending_turn[4];                                   // shift-held turns of the four knobs, in detents
static volatile u32 pending_click;                                     // bits 0..3: knob pressed; bits 4..7: with shift

extern "C" int veffturator_input(const Op1fInput* e) OP1F_NAMED("veffturator_input");
extern "C" int veffturator_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4 && e->shift) {         // shift held: the kept values are ours
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

// Our own form only: {"a":30,"c":0,"d":1,"f":30,"s":0,"v":1}
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

static const char NAMES[EXTRAS] = { 'a', 'f', 's', 'd', 'c' };

static void extras_read(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96];
    extras_start(p);
    if (op1f_value_get(&op1f_card_veffturator, where, which, KEY, text, sizeof text) > 0) {
        for (int k = 0; k < EXTRAS; ++k) { int v; if (read_member(text, NAMES[k], &v)) extra[p][k] = held(v, EXTRA_LEAST[k], EXTRA_MOST[k]); }
    }
    extra_ready |= 1u << p;
}

static u32 unsaved;                                                    // bit n: place n's values to be written

static void extras_write(u32 p) {
    unsigned where, which;
    where_of(p, &where, &which);
    char text[96], * q = text;
    q = put(q, "{\"a\":"); q = put_number(q, extra[p][E_AMOUNT]);
    q = put(q, ",\"c\":"); q = put_number(q, extra[p][E_COLOR]);
    q = put(q, ",\"d\":"); q = put_number(q, extra[p][E_DC]);
    q = put(q, ",\"f\":"); q = put_number(q, extra[p][E_FREQ]);
    q = put(q, ",\"s\":"); q = put_number(q, extra[p][E_SOFT]);
    put(q, ",\"v\":1}");
    if (op1f_value_set(&op1f_card_veffturator, where, which, KEY, text) == 0) unsaved &= ~(1u << p);   // else again next frame
}

// ---- the page: the common Veff page (veff_ui.h), painted every frame on the base's blank page
static char* put_db(char* q, int db) { if (db > 0) q = put(q, "+"); q = put_number(q, db); return put(q, " dB"); }

static const short QSIN[65] = { 0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767 };   // a quarter of sine, Q15
static inline int sine_of(u32 turn) {                                  // turn: 2^32 a turn; Q15
    const u32 i = turn >> 24;
    const u32 q = i & 63u;
    const int v = (i & 64u) ? QSIN[64 - q] : QSIN[q];
    return (i & 128u) ? -v : v;
}
static const short VIEW_OUT[37] = { 313, 324, 335, 346, 357, 369, 382, 394, 408, 421, 435, 450, 465, 481, 497, 513, 530, 548, 566,
                                    585, 605, 625, 646, 668, 690, 713, 737, 762, 787, 813, 841, 869, 898, 928, 959, 991, 1024 };   // 10^(dB/70), Q10
// The picture: four slanted "S" transfer curves of the real type and drive, side by side over the middle (white
// outlines, dim pink between them), wavering with the level and the colour; ring markers on the axis.
enum { CURVE_SPAN = 48, COLUMN_STEP = 8, ART_X0 = 24, ART_X1 = 456, PX_U = 40 };   // curve pitch; u per pixel, Q12
static const u32 RADIAN = 683565276u;                                  // a radian in turn units
static const u32 PER_MILLIRADIAN_SECOND = 22785u;                      // a milliradian a second, at 30 frames a second
static u32 frames_drawn, curve_turn, dot_turn;
static int view_env;                                                   // Q15, smoothed

using namespace veffui;
static inline int art_y(int y) { return y < VEFF_AXIS_Y - VEFF_ART_HALF ? VEFF_AXIS_Y - VEFF_ART_HALF : y > VEFF_AXIS_Y + VEFF_ART_HALF ? VEFF_AXIS_Y + VEFF_ART_HALF : y; }
static inline void marker(void* c, int x, int y, int r) { veffui::disc(c, x, y, r, 0); veffui::ring(c, x, y, r, r, VEFF_WHITE, 1, 4); }

static void paint_page(void* canvas, u32 where, u32 how, int shift) {
    int v[4] = { 6, 1, -3, 100 };
    for (int c = 0; c < 4; ++c) { int h = 0; if (op1f_param_heard(&op1f_card_veffturator, ASK_EFFECT, where, (unsigned)c, &h) == 0) v[c] = h; }
    v[V_DRIVE] = held(v[V_DRIVE], -36, 36); v[V_TYPE] = held(v[V_TYPE], 0, CURVES - 1);
    v[V_OUTPUT] = held(v[V_OUTPUT], -36, 0); v[V_MIX] = held(v[V_MIX], 0, 100);
    int x[EXTRAS];
    for (int k = 0; k < EXTRAS; ++k) x[k] = held(where < (u32)PLACES ? extra[where][k] : EXTRA_START[k], EXTRA_LEAST[k], EXTRA_MOST[k]);
    const int on = !(how & (u32)OP1F_SHOWN_OFF);

    // ---- the columns
    static char text[4][16];
    veffui::Column col[4];
    if (!shift) {
        col[0].label = "DRIVE";  put_db(text[0], v[V_DRIVE]);          col[0].frac = ((v[V_DRIVE] + 36) * 14564) >> 10;
        col[1].label = "TYPE";   put(text[1], TYPE_NAMES[v[V_TYPE]]);  col[1].frac = v[V_TYPE] * 205;
        col[2].label = "OUTPUT"; put_db(text[2], v[V_OUTPUT]);         col[2].frac = ((v[V_OUTPUT] + 36) * 29127) >> 10;
        col[3].label = "MIX";    put(put_number(text[3], v[V_MIX]), "%"); col[3].frac = (v[V_MIX] * 10486) >> 10;
    } else {
        col[0].label = "COLOR";
        if (!x[E_COLOR]) put(text[0], "off"); else { char* q = text[0]; if (x[E_AMOUNT] > 0) q = put(q, "+"); put_number(q, x[E_AMOUNT]); }
        col[0].frac = ((x[E_AMOUNT] + 100) * 5243) >> 10;
        col[1].label = "FREQ";
        const int hz = COLOR_HZ[x[E_FREQ]];
        if (hz < 1000) put(put_number(text[1], hz), " Hz");
        else { int r = hz + 50, k = 0, t = 0; while (r >= 1000) { r -= 1000; ++k; } while (r >= 100) { r -= 100; ++t; } char* q = put_number(text[1], k); q = put(q, "."); q = put_number(q, t); put(q, " kHz"); }
        col[1].frac = (x[E_FREQ] * 17477) >> 10;                       // / 60
        col[2].label = "CLIP"; put(text[2], x[E_SOFT] ? "on" : "off"); col[2].frac = x[E_SOFT] ? 1024 : 0;
        col[3].label = "DC";   put(text[3], x[E_DC] ? "on" : "off");   col[3].frac = x[E_DC] ? 1024 : 0;
    }
    for (int c = 0; c < 4; ++c) col[c].value = text[c];
    veffui::page(canvas, "URATOR", veffui::ICON_URATOR, col, shift ? "SHIFT" : 0, on);

    // ---- the motion
    const int* table = CURVE[v[V_TYPE]];
    const i32 gain = DRIVE_GAIN[v[V_DRIVE] + 36];
    const int mix = v[V_MIX];
    const int color = x[E_COLOR] ? x[E_AMOUNT] : 0;
    const int hz = COLOR_HZ[x[E_FREQ]];
    int pk = (int)(feed_peak >> 16) * 2;                               // min(1, level * 3)
    if (pk > 32767) pk = 32767;
    view_env += ((pk - view_env) * 61) >> 8;
    const int env = view_env;
    ++frames_drawn;
    curve_turn += (u32)(1300 + ((hz * 10923) >> 16)) * PER_MILLIRADIAN_SECOND;
    dot_turn += 2000u * PER_MILLIRADIAN_SECOND;
    const int line_k = on ? 60 + ((mix * 504) >> 8) : 60;             // brightness .23 + .77 mix
    const unsigned WHITE_LINE = veffui::white(line_k);
    int amp16 = (36 * 16 * VIEW_OUT[v[V_OUTPUT] + 36]) >> 10;          // pixels * 16 at full scale
    if (x[E_SOFT]) amp16 = (amp16 * 218) >> 8;
    const int lift = x[E_DC] ? 0 : 4;
    const int wobble = 287 + ((color * 3495) >> 10);                   // Q12
    const int axis = VEFF_AXIS_Y;
    veffui::axis(canvas);

    // the four curves, sampled per column: cy[j][i]
    enum { COLS = (ART_X1 - ART_X0) / COLUMN_STEP + 1 };
    static short cy[4][COLS];
    int centre[4];
    for (int j = 0; j < 4; ++j) {
        const int flutter = (((sine_of(curve_turn + (u32)j * RADIAN) * env) >> 15) * wobble) >> 15;   // Q12 of u
        centre[j] = VEFF_MID_X + (j * 2 - 3) * (CURVE_SPAN >> 1) - ((flutter * 26) >> 10);   // flutter in pixels (1/40 Q12)
        for (int i = 0; i < COLS; ++i) {
            int u = (ART_X0 + i * COLUMN_STEP - centre[j]) * PX_U;     // Q12
            u = u > 7 * 4096 ? 7 * 4096 : u < -7 * 4096 ? -7 * 4096 : u;
            const i32 y = shape(table, mulq(u << 16, gain));
            cy[j][i] = (short)art_y(axis + lift - (((y >> 16) * amp16) >> 19));
        }
    }
    // dim pink between neighbours, brighter in the middle gap
    static const int GAP_A[3] = { 70, 120, 70 };
    for (int j = 0; j < 3; ++j) {
        const int a = on ? (GAP_A[j] * (128 + mix + (env >> 8))) >> 8 : 40;
        const unsigned fill = veffui::pink(a > 256 ? 256 : a);
        for (int i = 0; i < COLS; ++i) {
            const int top = cy[j][i], bottom = cy[j + 1][i];          // the left curve is higher (it rose first)
            if (bottom - top > 1) op1f_paint_box(canvas, ART_X0 + i * COLUMN_STEP - (COLUMN_STEP >> 1), top + 1, COLUMN_STEP, bottom - top - 1, fill);
        }
    }
    for (int j = 0; j < 4; ++j) {
        const unsigned colour = (j == 1 || j == 2) ? WHITE_LINE : veffui::pink(line_k);
        for (int i = 1; i < COLS; ++i)
            op1f_paint_line(canvas, ART_X0 + (i - 1) * COLUMN_STEP, cy[j][i - 1], ART_X0 + i * COLUMN_STEP, cy[j][i], 1, colour);
    }
    // ring markers on the axis: two outside, one where each inner curve crosses it
    marker(canvas, VEFF_AXIS_L + 40, axis, 5);
    marker(canvas, VEFF_AXIS_R - 40, axis, 5);
    marker(canvas, centre[0], axis, 4);
    marker(canvas, centre[3], axis, 3);
    // a ring riding the second curve
    const int reach = 655 + ((env * 737) >> 15);                       // Q12 of u
    const int du = (sine_of(dot_turn) * reach) >> 15;
    const i32 dy = shape(table, mulq((du * 2) << 16, gain));
    const int rx = centre[1] + ((du * 2 * 26) >> 10), ry = art_y(axis + lift - (((dy >> 16) * amp16) >> 19));
    marker(canvas, rx, ry, 5);
    veffui::sparkles(canvas);
}

static u32 asked;

extern "C" void veffturator_frame(const Op1fFrame* now) OP1F_NAMED("veffturator_frame");
extern "C" void veffturator_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    {   // our own black page stands in for the effect page, so the base prints no name over it
        static void* page_doc; static u32 page_tried, page_stood;
        if (!page_tried) { page_tried = 1; page_doc = op1f_picture(&op1f_card_veffturator, "veffturatorpage.svg"); }
        if (page_doc && !page_stood) page_stood = (u32)op1f_page_stand_in(&op1f_card_veffturator, ASK_EFFECT, page_doc);
    }
    if (!asked) {
        asked = 1;
        for (u32 p = 0; p < (u32)PLACES; ++p) extras_start(p);
        op1f_log(3, LOG_FILE, 1, "veffturator: ready");
    }
    const u32 riding = (u32)op1f_riding(&op1f_card_veffturator, ASK_EFFECT);
    u32 todo = reread | (riding & ~extra_ready & ((1u << PLACES) - 1u));
    reread = 0;
    for (u32 p = 0; p < (u32)PLACES; ++p) if (todo >> p & 1u) extras_read(p);
    const u32 how = shown_how, where = shown_where;
    if ((how & (u32)OP1F_SHOWN_UP) && where < (u32)PLACES) {
        volatile int* x = extra[where];
        for (int k = 0; k < 4; ++k) {
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            if (k == 0) { x[E_AMOUNT] = held(x[E_AMOUNT] + t * 5, -100, 100); x[E_COLOR] = 1; }
            else if (k == 1) x[E_FREQ] = held(x[E_FREQ] + t, 0, FREQ_STEPS - 1);
            else x[k == 2 ? E_SOFT : E_DC] = t > 0 ? 1 : 0;
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 16u) { x[E_COLOR] ^= 1; unsaved |= 1u << where; }
        if (clicks & 64u) { x[E_SOFT] ^= 1; unsaved |= 1u << where; }
        if (clicks & 128u) { x[E_DC] ^= 1; unsaved |= 1u << where; }
        if (clicks & 1u) op1f_param_set(&op1f_card_veffturator, ASK_EFFECT, where, V_DRIVE, 0);
        if (clicks & 4u) op1f_param_set(&op1f_card_veffturator, ASK_EFFECT, where, V_OUTPUT, 0);
    } else {
        for (int k = 0; k < 4; ++k) pending_turn[k] = 0;
        pending_click = 0;
    }
    for (u32 p = 0; p < (u32)PLACES; ++p) if (unsaved >> p & 1u) extras_write(p);
    if (now->page == &op1f_card_veffturator && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP)) {
        paint_page(now->canvas, where, how, now->shift != 0);
    }
}
