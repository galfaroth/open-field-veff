// granular_core.h: veffgranular's grain DSP. No call of the unit's and none of the base's: the same code renders on
// the unit (veffgranular.cpp) and on a computer (tests/host_test.cpp).
//
// One voice is up to GRAINS grains. A grain reads the sample (through a GrainSource, grain_source.h) from a start
// frame forward at the note's speed, linearly interpolated, under a window from a table, and pans left/right.
// Grains start every `interval` output frames (size / overlap); where depends on the mode:
//   classic  the position (+ scan's drift) plus a random spray
//   loop     a play head that runs through a short region from the position and wraps; grains start at the head
//   cloud    as classic, but the spacing, the size, the pitch and the pan of each grain are randomised
// Integer only: no division, no float, no 64-bit multiply (mul_shift multiplies in 16-bit limbs).
//
// Never out of the sample: a grain is placed so its whole span fits inside [start, end), and every frame checks
// pos + 1 < end and pos >= start before it reads (the region may move under a held note when the sampler source
// arrives); a grain that fails is dropped, its last output fading out over TAIL_FRAMES (no click).
#pragma once
#include "grain_source.h"
#include "veffgranular_tables.h"

namespace gran {
typedef unsigned u32;
typedef int i32;

enum { FRAMES = 128, GRAINS = 4, TAIL_FRAMES = 64, MIN_INTERVAL = 64 };
// The cost cap: grains sounding at once in a voice, by the source's channels. Each grain-frame costs some 60
// instructions (stereo) or 45 (mono) on the unit plus its reads of the sample in DDR; 3 x 128 and 4 x 128 grain-frames
// are the most a render does. Lower these first if the unit's log shows renders over the declared cost.
enum { ACTIVE_MOST_STEREO = 3, ACTIVE_MOST_MONO = 4 };
enum { MODE_CLASSIC = 0, MODE_LOOP = 1, MODE_CLOUD = 2, MODES = 3 };
enum { LOOPS = 7 };                                   // loop length: free, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars
enum { V_POS, V_SIZE, V_DENSITY, V_SPRAY, V_SCAN, V_SPREAD, V_TUNE, V_VOLUME, VALUES };
enum { EV_BEGINS = 1, EV_ENDS = 2, EV_FRESH = 4 };   // mapped from the base's bits by the caller
// after a note is let go grains keep starting for the unit's release, at most this many blocks (10 s), then stop
enum { HOLD_BLOCKS = 6891 };
static const u32 MAX_INC = 8u << 16;                  // 8 source frames an output frame at most

struct Settings { u32 mode, window, loop; };          // the values beyond the knobs (kept with the sound)

struct Block {
    const int* values;   // VALUES numbers in the recipe's ranges
    u32 step;            // the note's phase step (level 9)
    u32 events;          // EV_*
    u32 period;          // a 96th of a beat in frames at 44,100, << 18; 0 when unknown
    u32 voice;           // 0..5, for the random seed
    i32* out;            // 2 * FRAMES words, left and right in turn, Q1.31
};

struct Grain {
    u32 pos, frac;       // whole frame, and the fraction Q16
    u32 inc;             // source frames an output frame, Q16
    u32 wphase, wstep;   // window phase over 2^32; wstep 0 marks a free grain
    u32 woff;            // the window's first point in WINDOW
    i32 gl, gr;          // gains Q15 (pan, overlap and volume)
    i32 last_l, last_r;  // the last frame it added, for the fade when it is dropped
};

struct Voice {
    Grain g[GRAINS];
    u32 sounding, released, next, rng, gen;
    u32 scan_q8;         // scan's drift, frames Q8, within the region
    u32 head, head_frac; // loop mode: the play head, frames from the loop's start, and its fraction Q16
    i32 tail_l, tail_r, tail_dl, tail_dr;
    u32 tail_n;
};

// what the page draws: written by the render, one word each, read by the frame function
struct Vis { u32 pos[GRAINS]; u32 level[GRAINS]; };

// (a * b) >> shift of the full 64-bit product, in 16-bit limbs (no 64-bit multiply on the unit)
static inline u32 mul_shift(u32 a, u32 b, unsigned shift) {
    const u32 al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = b >> 16;
    const u32 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const u32 mid = (ll >> 16) + (lh & 0xffffu) + (hl & 0xffffu);
    const u32 low = (mid << 16) | (ll & 0xffffu);
    const u32 high = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    if (shift == 0) return low;
    if (shift >= 32) return high >> (shift - 32);
    return (low >> shift) | (high << (32 - shift));
}
static inline u32 rnd(Voice& v) { return v.rng = v.rng * 1664525u + 1013904223u; }
static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline i32 sat_shl4(i32 v) { return v > 0x07ffffff ? 0x7fffffff : v < -0x08000000 ? (i32)0x80000000 : (i32)((u32)v << 4); }
// a + b within [0, m), a < m, b < m
static inline u32 wrap_add(u32 a, u32 b, u32 m) { return a >= m - b ? a - (m - b) : a + b; }

// inc (Q16) moved by off64 64ths of a semitone, held to 1 .. MAX_INC
static inline u32 pitch(u32 inc, i32 off64) {
    int oct = 0;
    while (off64 < 0 && oct > -12) { off64 += 768; --oct; }
    while (off64 >= 768 && oct < 12) { off64 -= 768; ++oct; }
    if (off64 < 0) off64 = 0;
    if (off64 >= 768) off64 = 767;
    const u32 m = mul_shift(SEMI_Q30[off64 >> 6], FINE_Q30[off64 & 63], 30);   // 1..2, Q30
    u32 r = mul_shift(inc, m, 30);
    for (; oct > 0; --oct) { if (r >= MAX_INC) return MAX_INC; r <<= 1; }
    for (; oct < 0; ++oct) r >>= 1;
    return r < 1u ? 1u : r > MAX_INC ? MAX_INC : r;
}

static inline void drop(Voice& v, Grain& g) {        // a grain leaves; what it last gave fades over TAIL_FRAMES
    if (!g.wstep) return;
    g.wstep = 0;
    v.tail_l += g.last_l; v.tail_r += g.last_r;
    v.tail_dl = v.tail_l >> 6; v.tail_dr = v.tail_r >> 6;   // TAIL_FRAMES = 64
    v.tail_n = TAIL_FRAMES;
}
static inline void drop_all(Voice& v) { for (int k = 0; k < GRAINS; ++k) drop(v, v.g[k]); }

static inline void voice_clear(Voice& v, u32 voice) {
    for (int k = 0; k < GRAINS; ++k) { v.g[k].wstep = 0; v.g[k].last_l = v.g[k].last_r = 0; }
    v.sounding = v.released = v.next = v.scan_q8 = v.head = v.head_frac = 0;
    v.tail_l = v.tail_r = v.tail_dl = v.tail_dr = 0; v.tail_n = 0;
    v.rng = 0x9e3779b9u + voice * 0x61c88647u;
    v.gen = 0;
}

// the per-block numbers shared by the spawns
struct Plan {
    u32 start, end, len;          // region
    u32 inc;                      // Q16 at the note, before a grain's detune
    i32 off64;                    // root and tune, 64ths of a semitone
    u32 size, mode, woff;
    u32 interval;
    i32 gain;                     // Q15
    u32 spray;                    // frames either side
    i32 spread;                   // 0..100
    u32 base;                     // frames from start where grains start (before spray)
    u32 active_most;              // the cost cap
};

static inline void spawn(Voice& v, const Plan& p) {
    Grain* g = 0;
    u32 active = 0;
    for (int k = 0; k < GRAINS; ++k) { if (v.g[k].wstep) ++active; else if (!g) g = &v.g[k]; }
    if (!g || active >= p.active_most) return;
    u32 rel = p.base;
    u32 spray = p.spray;
    if (p.mode == MODE_CLOUD) { const u32 least = p.len >> 4; if (spray < least) spray = least; if (spray > p.len >> 1) spray = p.len >> 1; }
    if (spray) {
        u32 off = mul_shift(rnd(v), 2u * spray + 1u, 32);                 // 0 .. 2 spray (<= len)
        while (off >= p.len) off -= p.len;
        rel = wrap_add(rel, off, p.len);                          // + off
        rel = wrap_add(rel, p.len - spray, p.len);                           // - spray
    }
    int sz = (int)p.size;
    if (p.mode == MODE_CLOUD) sz = clampi(sz + (int)(rnd(v) >> 27) - 16, 0, 100);
    // detune between grains: a quarter semitone at spread 100 (a whole one in cloud)
    const i32 d = ((i32)(rnd(v) >> 16) - 32768) * p.spread;               // +-3,276,800
    const i32 detune = p.mode == MODE_CLOUD ? (d * 5) >> 18 : (d * 5) >> 20;
    const u32 inc = pitch(p.inc, p.off64 + detune);
    u32 wstep = SIZE_WSTEP[sz];
    u32 span = mul_shift(SIZE_FRAMES[sz], inc, 16) + 2u;
    while (span >= p.len && wstep < 0x40000000u) { wstep <<= 1; span = (span >> 1) + 1u; }   // a sample shorter than a grain
    u32 at = p.start + rel;
    if (span < p.len) { if (at + span >= p.end) at = p.end - 1u - span; }
    else at = p.start;
    i32 pan = 0;
    if (p.spread) pan = (((i32)(rnd(v) >> 17) - 16384) * (p.spread * 327)) >> 14;   // +-32700
    g->pos = at; g->frac = 0; g->inc = inc;
    g->wphase = 0; g->wstep = wstep; g->woff = p.woff;
    g->gl = (p.gain * (pan > 0 ? 32767 - pan : 32767)) >> 15;
    g->gr = (p.gain * (pan < 0 ? 32767 + pan : 32767)) >> 15;
    g->last_l = g->last_r = 0;
}

template <int STEREO>
static inline void mix(Voice& v, const GrainSource& s, i32* out, u32 count) {
    const short* data = s.data;
    const u32 start = s.start, last = s.end - 1u;     // a read of pos and pos + 1 needs pos < last
    for (int k = 0; k < GRAINS; ++k) {
        Grain& g = v.g[k];
        if (!g.wstep) continue;
        u32 pos = g.pos, frac = g.frac, ph = g.wphase;
        const u32 ii = g.inc >> 16, fi = g.inc & 0xffffu, ws = g.wstep;
        const unsigned short* win = WINDOW + g.woff;
        const i32 gl = g.gl, gr = g.gr;
        i32 cl = g.last_l, cr = g.last_r;
        i32* o = out;
        u32 n = count;
        for (; n; --n) {
            if (pos >= last || pos < start) { g.last_l = cl; g.last_r = cr; drop(v, g); break; }
            const i32 f = (i32)(frac >> 1);                                  // Q15
            const i32 w = win[ph >> 22];
            i32 l, r;
            if (STEREO) {
                const short* at = data + 2u * pos;
                l = at[0] + (((at[2] - at[0]) * f) >> 15);
                r = at[1] + (((at[3] - at[1]) * f) >> 15);
            } else {
                const short* at = data + pos;
                l = r = at[0] + (((at[1] - at[0]) * f) >> 15);
            }
            cl = (l * ((w * gl) >> 15)) >> 3;
            cr = (r * ((w * gr) >> 15)) >> 3;
            o[0] += cl; o[1] += cr; o += 2;
            frac += fi;
            pos += ii + (frac >> 16);
            frac &= 0xffffu;
            const u32 nph = ph + ws;
            if (nph < ph) { g.wstep = 0; cl = cr = 0; break; }               // the window ended at 0
            ph = nph;
        }
        if (g.wstep) { g.pos = pos; g.frac = frac; g.wphase = ph; g.last_l = cl; g.last_r = cr; }
    }
}

// One voice's block. `src` is the caller's copy of the source, taken once for this call.
static inline void render(Voice& v, const GrainSource& src, const Settings& set, const Block& b, Vis* vis) {
    i32* out = b.out;
    for (u32 i = 0; i < 2u * FRAMES; ++i) out[i] = 0;
    if (b.events & EV_FRESH) voice_clear(v, b.voice);
    const int ok = grain_source_ok(src);
    if (b.events & EV_BEGINS) {
        drop_all(v);                                    // a voice handed a new note: its old grains fade
        v.sounding = 1; v.released = 0; v.next = 0; v.scan_q8 = 0; v.head = 0; v.head_frac = 0;
    }
    if (b.events & EV_ENDS) v.released = 1;
    if (!ok || v.gen != src.generation) {               // nothing to play, or other frames: the grains fade
        drop_all(v);
        v.gen = ok ? src.generation : 0;
        v.scan_q8 = 0; v.head = 0; v.head_frac = 0;
    }
    if (ok && v.sounding) {
        const int* val = b.values;
        Plan p;
        p.start = src.start; p.end = src.end; p.len = src.end - src.start;
        p.mode = set.mode < (u32)MODES ? set.mode : 0u;
        p.active_most = src.stereo ? (u32)ACTIVE_MOST_STEREO : (u32)ACTIVE_MOST_MONO;
        p.woff = (set.window < (u32)WINDOW_SHAPES ? set.window : 0u) * (u32)WINDOW_POINTS;
        p.size = (u32)clampi(val[V_SIZE], 0, 100);
        const int dens = clampi(val[V_DENSITY], 0, 100);
        const int vol = clampi(val[V_VOLUME], 0, 100);
        p.spread = clampi(val[V_SPREAD], 0, 100);
        // the speed at the note: step against C4 at 44,100, then the sample's rate
        u32 inc = mul_shift(b.step, INV_STEP_C4_Q40, 40);
        inc = mul_shift(inc, src.rate_q16, 16);
        p.inc = inc ? inc : 1u;
        p.off64 = (60 - (i32)src.root + clampi(val[V_TUNE], -24, 24)) * 64;
        const u32 inc_note = pitch(p.inc, p.off64);
        p.interval = mul_shift(SIZE_FRAMES[p.size], INV_OVERLAP_Q16[dens], 16);
        if (p.interval < (u32)MIN_INTERVAL) p.interval = MIN_INTERVAL;
        p.gain = (i32)((OVERLAP_GAIN_Q15[dens] * (u32)(vol * 327)) >> 15);
        u32 spray = (u32)clampi(val[V_SPRAY], 0, 100) * 882u;            // up to 2 s at 44.1 kHz
        spray = mul_shift(spray, src.rate_q16, 16);
        if (spray > p.len >> 1) spray = p.len >> 1;
        p.spray = spray;
        // scan: -100..100 is -2..+2 times the sample's own speed, moving the position through the region
        const int sc = clampi(val[V_SCAN], -100, 100);
        const u32 lq = p.len << 8;
        if (v.scan_q8 >= lq) v.scan_q8 = 0;
        if (sc) {
            u32 delta = mul_shift((u32)(sc < 0 ? -sc : sc) * src.rate_q16, 21474836u, 32);   // sc/100 * rate * 128 frames, Q8
            while (delta >= lq) delta -= lq;
            v.scan_q8 = sc > 0 ? wrap_add(v.scan_q8, delta, lq) : wrap_add(v.scan_q8, lq - delta, lq);
            if (v.scan_q8 >= lq) v.scan_q8 = 0;
        }
        const u32 pos = (u32)clampi(val[V_POS], 0, 1000);
        u32 base = mul_shift(p.len, pos * 4294967u, 32);
        if (base >= p.len) base = p.len - 1u;
        base = wrap_add(base, v.scan_q8 >> 8, p.len);
        if (p.mode == MODE_LOOP) {
            const u32 span = mul_shift(SIZE_FRAMES[p.size], inc_note, 16) + 2u;
            u32 loop = 4u * span;
            static const unsigned short N96[LOOPS] = { 0, 6, 12, 24, 48, 96, 192 };
            const u32 l = set.loop < (u32)LOOPS ? set.loop : 0u;
            if (l && b.period) loop = mul_shift(2u * N96[l] * (b.period >> 8), inc_note, 26);   // out frames * inc
            if (loop < 64u) loop = 64u;
            if (loop > p.len) loop = p.len;
            const u32 adv = inc_note * (u32)FRAMES;                          // Q16, at most 2^26
            v.head_frac += adv & 0xffffu;
            v.head += (adv >> 16) + (v.head_frac >> 16);
            v.head_frac &= 0xffffu;
            while (v.head >= loop) v.head -= loop;
            base = wrap_add(base, v.head, p.len);
        }
        p.base = base;
        const int spawning = !(v.released && v.released > (u32)HOLD_BLOCKS);
        u32 at = 0;
        while (at < (u32)FRAMES) {
            if (spawning && v.next == 0) {
                spawn(v, p);
                u32 iv = p.interval;
                if (p.mode == MODE_CLOUD) iv = mul_shift(iv, 16384u + (rnd(v) >> 17) * 3u, 15);   // 0.5 .. 2 times
                v.next = iv < (u32)MIN_INTERVAL ? (u32)MIN_INTERVAL : iv;
            }
            u32 count = (u32)FRAMES - at;
            if (spawning && v.next < count) count = v.next;
            if (src.stereo) mix<1>(v, src, out + 2u * at, count); else mix<0>(v, src, out + 2u * at, count);
            at += count;
            if (spawning) v.next -= count;
        }
        if (v.released) { ++v.released; if (v.released > (u32)HOLD_BLOCKS + 1u) v.released = HOLD_BLOCKS + 1u; }
        if (!spawning) {                                                    // past the hold: done once the grains end
            int any = 0;
            for (int k = 0; k < GRAINS; ++k) any |= v.g[k].wstep != 0;
            if (!any) v.sounding = 0;
        }
    } else if (ok) {
        if (src.stereo) mix<1>(v, src, out, FRAMES); else mix<0>(v, src, out, FRAMES);   // grains left of a note
    }
    if (v.tail_n) {                                                         // the fade of dropped grains
        for (u32 i = 0; i < (u32)FRAMES && v.tail_n; ++i) {
            out[2 * i] += v.tail_l; out[2 * i + 1] += v.tail_r;
            v.tail_l -= v.tail_dl; v.tail_r -= v.tail_dr;
            if (--v.tail_n == 0) v.tail_l = v.tail_r = v.tail_dl = v.tail_dr = 0;
        }
    }
    for (u32 i = 0; i < 2u * FRAMES; ++i) out[i] = sat_shl4(out[i]);
    if (vis) for (int k = 0; k < GRAINS; ++k) {
        const Grain& g = v.g[k];
        vis->pos[k] = g.pos;
        vis->level[k] = g.wstep ? WINDOW[g.woff + (g.wphase >> 22)] + 1u : 0u;
    }
}

}  // namespace gran
