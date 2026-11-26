// veffchop_engine.h: the chop itself, with nothing of the unit in it, so that the computer can test it.
//
//   Analysis   once per sample, a bounded piece a frame (step()): for every window of WIN frames (5 ms) the sum of
//              |L|+|R|, where the sum L+R first and last crosses zero in it, how often it crosses, and the peak of
//              each of the page's columns. Then (finish()) the candidate onsets with their strengths, 0..1024.
//   Slicing    from the candidates and the knobs (slice()), with no look at the sample again: TRANSIENT (strength
//              over a threshold set by SENSITIVITY), LAZY (the same, cuts closer than GAP merged, each cut snapped
//              to a zero crossing), EQUAL (N parts).
//   Mapping    the slices onto the 24 keys (map()): ORDER or KIT, each with or without WRAP.
//   Writing    the Q31 start/end pairs (targets()), and the order of the 48 word writes (plan()) such that no state
//              between two writes has a start past its end.
//
// Integer maths only. No division instruction (udiv is a bit loop), no 64-bit type, no struct copied or set whole
// (the unit has no memcpy or memset).
#pragma once

namespace chop {

typedef unsigned u32;
typedef unsigned short u16;
typedef unsigned char u8;

enum {
    WIN = 220,                 // frames a window: 5 ms
    ENV_MAX = 4096,            // windows: 882000 / 220 = 4009
    COLS = 228,                // the page's wave columns, 2 px each
    KEYS = 24,
    MAX_CAND = 1024,
    MAX_SLICES = 24,
    CAPACITY = 882000,         // frames the stereo fractions are fractions of
    MONO_CAPACITY = 529200,    // v6: and the mono ones (the render's capacity: 0xd7550 with the stereo byte set, 0x81330 without)
    MIN_Q = 0x00100000,        // the stock edit's least length, Q31
    MIN_FRAMES = 882,          // 20 ms: no slice shorter (well over MIN_Q's 431 frames)
    BUDGET = 4096,             // words of the sample read a frame at most
    FINISH_BUDGET = 256,       // windows finish_step() looks at a frame, in each of its two passes (v2)
    LOCAL = 6,
    PRE = 64,                  // frames a cut lies before the first loud frame                 // a candidate is the largest rise within +-6 windows (30 ms)
    NONE = 255,
};

enum Mode { TRANSIENT = 0, LAZY = 1, EQUAL = 2, MODES = 3 };
enum Map { ORDER = 0, ORDER_WRAP = 1, KIT = 2, KIT_WRAP = 3, MAPS = 4 };

// ---------------------------------------------------------------- integer helpers

// n / d by a bit loop (d > 0, d < 2^31).
static inline u32 udiv(u32 n, u32 d) {
    if (!d) return 0xffffffffu;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}

// floor(f * 2^31 / cap), the frame's fraction of the capacity (882000 stereo, 529200 mono), exact: the 64-bit
// numerator held as two words and divided by a bit loop. Past the capacity it is 0x7fffffff.
static inline u32 capacity_of(u32 stereo) { return stereo ? (u32)CAPACITY : (u32)MONO_CAPACITY; }
static inline u32 q31_of_frame(u32 f, u32 cap = (u32)CAPACITY) {
    if (f >= cap) return 0x7fffffffu;
    const u32 hi = f >> 1, lo = (f & 1u) << 31;
    u32 q = 0, r = 0;
    for (int i = 63; i >= 0; --i) {
        const u32 bit = i >= 32 ? (hi >> (i - 32)) & 1u : (lo >> i) & 1u;
        r = (r << 1) | bit;                          // r < 2 * cap
        if (r >= cap) { r -= cap; if (i < 32) q |= 1u << i; }   // f < capacity: the quotient < 2^31
    }
    return q;
}

// floor(q * cap / 2^31) to within one frame (for the page and the tests): q * (cap / 16) >> 27 in two halves (both
// capacities are a multiple of 16: 55125 and 33075).
static inline u32 frame_of_q31(u32 q, u32 cap = (u32)CAPACITY) {
    const u32 k = cap >> 4;
    const u32 a = (q >> 16) * k, b = (q & 0xffffu) * k;              // q * k = a * 2^16 + b
    return (a >> 11) + ((((a & 0x7ffu) << 15) + (b >> 1)) >> 26);
}

// ---------------------------------------------------------------- analysis

struct Analysis {
    u32 frames, windows, read;          // the sample's frames, its windows, frames read so far
    u32 stereo;                         // v6: 1 a stereo sample (a word a frame), 0 a mono one (a halfword a frame)
    u32 complete;
    u32 col_scale;                      // (COLS << 20) / frames
    // the window being summed
    u32 acc, in_win, w, first, last, count, jthr, jat;
    int prev_sign;
    u32 peak_env, words_read_last;      // the largest window sum; words read by the last step
    u32 env[ENV_MAX];
    u8 zfirst[ENV_MAX], zlast[ENV_MAX], zcount[ENV_MAX];
    u8 jump[ENV_MAX];                   // the first frame in the window louder than 4x the window before's mean, or NONE
    u16 col[COLS];                      // peak |sample| of each column, 0..32768
    // candidates, in time order
    u32 ncand, max_rise;
    u32 cpos[MAX_CAND];                 // the cut, frames
    u16 cstr[MAX_CAND];                 // 0..1024
    u32 rise[ENV_MAX];                  // scratch for finish()
    u32 fpass, fat;                     // finish_step(): the pass (0 rises, 1 candidates, 2 done) and the window reached
};

static inline void begin(Analysis& a, u32 frames, u32 stereo = 1) {
    a.frames = frames;
    a.stereo = stereo ? 1u : 0u;
    a.windows = udiv(frames + WIN - 1, WIN);
    if (a.windows > ENV_MAX) a.windows = ENV_MAX;
    a.read = 0; a.complete = 0;
    a.col_scale = frames ? udiv((u32)COLS << 20, frames) : 0;
    a.acc = 0; a.in_win = 0; a.w = 0; a.first = NONE; a.last = NONE; a.count = 0; a.prev_sign = 0; a.jthr = 300; a.jat = NONE;
    a.peak_env = 0; a.words_read_last = 0; a.ncand = 0; a.max_rise = 0; a.fpass = 0; a.fat = 0;
    for (u32 i = 0; i < (u32)COLS; ++i) a.col[i] = 0;
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

static inline void close_window(Analysis& a) {
    if (a.w < (u32)ENV_MAX) {
        a.env[a.w] = a.acc;
        a.zfirst[a.w] = (u8)a.first; a.zlast[a.w] = (u8)a.last;
        a.zcount[a.w] = (u8)(a.count > 254 ? 254 : a.count);
        a.jump[a.w] = (u8)a.jat;
        if (a.acc > a.peak_env) a.peak_env = a.acc;
    }
    a.jthr = (a.acc >> 6) + 300;                                    // 4x this window's mean |L|+|R| (220 ~ 2^6 * 3.4)
    ++a.w; a.acc = 0; a.in_win = 0; a.first = NONE; a.last = NONE; a.count = 0; a.jat = NONE;
}

// Reads at most `budget` words (stereo frames) of the sample through rd(addr), from data + read * 4. Answers 1 when
// the whole sample has been read (then finish()).
typedef u32 (*Reader)(u32 addr);
static inline int step(Analysis& a, Reader rd, u32 data, u32 budget) {
    u32 n = a.frames - a.read;
    if (n > budget) n = budget;
    for (u32 k = 0; k < n; ++k) {
        const u32 f = a.read + k;
        int L, R;
        if (a.stereo) {
            const u32 v = rd(data + f * 4u);
            L = (short)(v & 0xffffu); R = (short)(v >> 16);
        } else {                                                    // v6: mono, a halfword a frame: the word holding it read
            const u32 v = rd(data + (f & ~1u) * 2u);
            L = R = (short)((f & 1u) ? (v >> 16) : (v & 0xffffu));
        }
        const u32 mag = (u32)(iabs(L) + iabs(R));
        a.acc += mag;
        if (a.jat == (u32)NONE && mag > a.jthr) a.jat = a.in_win;
        const int s = L + R >= 0 ? 1 : -1;
        if (f && s != a.prev_sign) {
            if (a.first == NONE) a.first = a.in_win;
            a.last = a.in_win;
            ++a.count;
        }
        a.prev_sign = s;
        const u32 c = (f * a.col_scale) >> 20;
        const u32 pk = (u32)(iabs(L) > iabs(R) ? iabs(L) : iabs(R));
        if (c < (u32)COLS && pk > a.col[c]) a.col[c] = (u16)pk;
        if (++a.in_win == (u32)WIN) close_window(a);
    }
    a.read += n;
    a.words_read_last = n;
    if (a.read >= a.frames) {
        if (a.in_win) close_window(a);
        a.complete = 1;
        return 1;
    }
    return 0;
}

// 0..100 of the sample read.
static inline u32 progress(const Analysis& a) { return a.frames ? udiv((a.read >> 4) * 100u, (a.frames >> 4) + 1) : 0; }

// The candidate onsets: where two windows' sum rises over the two before by half again, and the rise is the largest
// within +-LOCAL windows. Its strength is the rise against the largest rise, 0..1024.
// v2: spread over frames, at most `budget` windows a call in each pass (the first pass the rises, the second the
// candidates). Answers 1 when the candidates are all there. The same result as finish() in one go.
static inline int finish_step(Analysis& a, u32 budget) {
    const u32 nw = a.w < a.windows ? a.w : a.windows;
    const u32 floor = a.peak_env >> 6;                              // under -36 dB of the loudest window: no onset
    if (a.fpass == 0) {
        if (a.fat == 0) a.max_rise = 0;
        u32 end = a.fat + budget;
        if (end > nw) end = nw;
        for (u32 i = a.fat; i < end; ++i) {
            const u32 d = a.env[i] + (i + 1 < nw ? a.env[i + 1] : a.env[i]);
            const u32 p = (i >= 1 ? a.env[i - 1] : 0) + (i >= 2 ? a.env[i - 2] : 0);
            u32 r = 0;
            if (d > p && (d >> 1) > floor && (d >> 1) + (d >> 2) > p) r = d - p;   // d > p * 4/3 at the least, cheap
            if (i == 0 && d) r = d;                                 // the sample's start counts as a hit
            a.rise[i] = r;
            if (r > a.max_rise) a.max_rise = r;
        }
        a.fat = end;
        if (a.fat < nw) return 0;
        a.fpass = 1; a.fat = 0; a.ncand = 0;
        return 0;
    }
    if (a.fpass == 1) {
        u32 end = a.fat + budget;
        if (end > nw) end = nw;
        for (u32 i = a.fat; i < end && a.ncand < (u32)MAX_CAND; ++i) {
            const u32 r = a.rise[i];
            if (!r) continue;
            int top = 1;
            const u32 lo = i > (u32)LOCAL ? i - LOCAL : 0, hi = i + LOCAL < nw ? i + LOCAL : nw - 1;
            for (u32 j = lo; j <= hi && top; ++j) {
                if (j < i && a.rise[j] >= r) top = 0;              // the earlier of two equal rises wins
                if (j > i && a.rise[j] > r) top = 0;
            }
            if (!top) continue;
            // the largest rise can lie a window or two after the attack began: the cut goes to the earliest of the two
            // windows before it whose sum at least doubles the one before (and stands over the floor)
            u32 at = i;
            for (u32 j = i >= 2 ? i - 2 : 0; j < i; ++j) {
                const u32 before = j ? a.env[j - 1] : 0;
                if (a.env[j] > (floor >> 2) && a.env[j] > 2 * before + (floor >> 4)) { at = j; break; }
            }
            u32 s = a.max_rise ? udiv((r >> 6) * 1024u, (a.max_rise >> 6) + 1) : 0;   // sums < 2^24: (r>>6)*1024 fits
            if (s > 1024) s = 1024;
            if (!s) s = 1;
            // to the frame: the first loud frame of that window, or of the window before when it lies at its very end
            u32 pos = at * WIN + (a.jump[at] != (u8)NONE ? a.jump[at] : 0);
            if ((a.jump[at] == (u8)NONE || a.jump[at] < 8) && at && a.jump[at - 1] != (u8)NONE && a.jump[at - 1] + 40u > (u32)WIN)
                pos = (at - 1) * WIN + a.jump[at - 1];
            pos = pos > (u32)PRE ? pos - PRE : 0;
            a.cpos[a.ncand] = pos;
            a.cstr[a.ncand] = (u16)s;
            ++a.ncand;
        }
        a.fat = end;
        if (a.fat < nw && a.ncand < (u32)MAX_CAND) return 0;
        a.fpass = 2;
    }
    return 1;
}

static inline void finish(Analysis& a) { a.fpass = 0; a.fat = 0; while (!finish_step(a, 0x7fffffffu)) {} }

// ---------------------------------------------------------------- slicing

struct Params {
    u32 mode, sens, parts, map, gap_ms, offset_ms;   // sens 0..100, parts 2..24, gap 20..500, offset 0..20
};

struct Slices {
    u32 n;
    u32 start[MAX_SLICES];              // frames, ascending, start[0] = 0
    u32 end[MAX_SLICES];                // the last frame of each
};

// The least strength a candidate needs at a sensitivity, 0..100: 100 takes nearly every candidate, 0 the loudest.
static inline u32 threshold(u32 sens) {
    if (sens > 100) sens = 100;
    const u32 k = 100 - sens;
    return 4 + ((k * k * 105u) >> 10);                              // (k/100)^2 of 1024, at least 4
}

static inline u32 frames_of_ms(u32 ms) { return ms * 44u + ((ms * 26u) >> 8); }   // 44.1 a ms

// Nearest zero crossing of the sum to frame b, from the windows around it; b itself when none is near.
static inline u32 snap(const Analysis& a, u32 b) {
    const u32 j = udiv(b, WIN);
    u32 best = b, dist = 0xffffffffu;
    for (int dj = -1; dj <= 1; ++dj) {
        const int wj = (int)j + dj;
        if (wj < 0 || (u32)wj >= a.w || (u32)wj >= (u32)ENV_MAX) continue;
        const u32 base = (u32)wj * WIN;
        for (int t = 0; t < 2; ++t) {
            const u32 z = t ? a.zlast[wj] : a.zfirst[wj];
            if (z == (u32)NONE) continue;
            const u32 p = base + z;
            const u32 d = p > b ? p - b : b - p;
            if (d < dist) { dist = d; best = p; }
        }
    }
    return best;
}

// The candidates kept at a threshold, at most `most` of them: the strongest, ties to the earlier. keep[i] set.
static u32 hist[1025];                 // scratch of slice(), kept off the stack
static u8 keep[MAX_CAND], pre[MAX_CAND];

static inline u32 select(const Analysis& a, u32 thr, u32 most, u8* keep) {
    for (u32 i = 0; i < 1025u; ++i) hist[i] = 0;
    u32 total = 0;
    for (u32 i = 0; i < a.ncand; ++i) { keep[i] = 0; if (a.cstr[i] >= thr) { ++hist[a.cstr[i]]; ++total; } }
    u32 cut = thr, room = most;
    if (total > most) {                                             // the least strength that keeps at most `most`
        u32 above = 0;
        int s = 1024;
        for (; s >= (int)thr; --s) { if (above + hist[s] > most) break; above += hist[s]; }
        cut = (u32)s + 1;                                           // all above s kept; of s itself, `room` more
        room = most - above;
        for (u32 i = 0; i < a.ncand; ++i) if (a.cstr[i] >= cut) keep[i] = 1;
        for (u32 i = 0; i < a.ncand && room; ++i) if (a.cstr[i] == (u32)s) { keep[i] = 1; --room; }
        return most;
    }
    for (u32 i = 0; i < a.ncand; ++i) if (a.cstr[i] >= thr) keep[i] = 1;
    return total;
}

static inline void ends(Slices& s, u32 frames) {
    for (u32 k = 0; k < s.n; ++k) s.end[k] = k + 1 < s.n ? s.start[k + 1] - 1 : (frames > 20 ? frames - 20 : frames - 1);
}

static inline void slice(const Analysis& a, const Params& p, Slices& s) {
    const u32 frames = a.frames;
    s.n = 0;
    if (frames < (u32)MIN_FRAMES * 2) { s.n = 1; s.start[0] = 0; ends(s, frames); return; }
    if (p.mode == EQUAL) {
        u32 n = p.parts < 2 ? 2 : p.parts > 24 ? 24 : p.parts;
        while (n > 1 && udiv(frames, n) < (u32)MIN_FRAMES) --n;
        for (u32 k = 0; k < n; ++k) s.start[k] = udiv(k * frames, n);   // k * frames < 24 * 882000
        s.n = n;
        ends(s, frames);
        return;
    }
    const u32 thr = threshold(p.sens);
    const u32 off = frames_of_ms(p.offset_ms > 20 ? 20 : p.offset_ms);
    const u32 gap = p.mode == LAZY ? frames_of_ms(p.gap_ms < 20 ? 20 : p.gap_ms > 500 ? 500 : p.gap_ms) : (u32)MIN_FRAMES;
    if (p.mode == LAZY) {
        // merged first (in time order, a cut within GAP of the last kept one is dropped), then at most 23 kept
        u32 last = 0, have = 0;
        for (u32 i = 0; i < a.ncand; ++i) {
            pre[i] = 0;
            if (a.cstr[i] < thr) continue;
            const u32 b = a.cpos[i];
            if (b < gap) continue;                                  // the sample's start is slice 1 already
            if (have && b - last < gap) continue;
            if (frames - b < gap) continue;                         // too short a last slice: merged into the one before
            pre[i] = 1; last = b; have = 1;
        }
        u32 cnt = 0;
        for (u32 i = 0; i < a.ncand; ++i) cnt += pre[i];
        if (cnt > (u32)MAX_SLICES - 1) {                            // the weakest dropped: a histogram of the kept
            Analysis const& aa = a;
            for (u32 i = 0; i < 1025u; ++i) hist[i] = 0;
            for (u32 i = 0; i < aa.ncand; ++i) if (pre[i]) ++hist[aa.cstr[i]];
            u32 above = 0; int sidx = 1024;
            for (; sidx >= 0; --sidx) { if (above + hist[sidx] > (u32)MAX_SLICES - 1) break; above += hist[sidx]; }
            u32 room = MAX_SLICES - 1 - above;
            for (u32 i = 0; i < aa.ncand; ++i) {
                if (!pre[i]) { keep[i] = 0; continue; }
                if (aa.cstr[i] > (u32)sidx) keep[i] = 1;
                else if (aa.cstr[i] == (u32)sidx && room) { keep[i] = 1; --room; }
                else keep[i] = 0;
            }
        } else for (u32 i = 0; i < a.ncand; ++i) keep[i] = pre[i];
    } else {
        select(a, thr, MAX_SLICES, keep);
    }
    s.start[0] = 0; s.n = 1;
    for (u32 i = 0; i < a.ncand && s.n < (u32)MAX_SLICES; ++i) {
        if (!keep[i]) continue;
        u32 b = a.cpos[i];
        b = b > off ? b - off : 0;
        if (p.mode == LAZY) b = snap(a, b);
        if (b < s.start[s.n - 1] + (u32)MIN_FRAMES) continue;      // too close to the cut before (or the start)
        if (b + (u32)MIN_FRAMES > frames) continue;
        s.start[s.n++] = b;
    }
    ends(s, frames);
}

// ---------------------------------------------------------------- mapping

// The stock kit's layout: kick 0, kick2 1, snare 2, snare2 3, ..., chh 8, ohh 10.
// six slices or more: both kicks, both hats, then both snares from what is left; fewer: kick, hat, snare, kick2, ohh
static const int KIT_ORDER6[6] = { 0, 1, 8, 10, 2, 3 }, KIT_ROLE6[6] = { 0, 0, 1, 1, 2, 2 };
static const int KIT_ORDER5[5] = { 0, 8, 2, 1, 10 }, KIT_ROLE5[5] = { 0, 1, 2, 0, 1 };   // 0 low/strong, 1 bright/short, 2 middle

struct Features { u32 bright[MAX_SLICES], strong[MAX_SLICES], len[MAX_SLICES]; };

static inline void features(const Analysis& a, const Slices& s, Features& f) {
    for (u32 k = 0; k < s.n; ++k) {
        const u32 w0 = udiv(s.start[k], WIN), w1e = udiv(s.end[k], WIN) + 1;
        u32 w1 = w0 + 8 < w1e ? w0 + 8 : w1e;
        if (w1 > a.w) w1 = a.w;
        if (w1 > (u32)ENV_MAX) w1 = ENV_MAX;
        u32 z = 0, e = 0, n = 0;
        for (u32 w = w0; w < w1; ++w) { z += a.zcount[w]; if (a.env[w] > e) e = a.env[w]; ++n; }
        f.bright[k] = n ? udiv(z * 64u, n) : 0;
        f.strong[k] = e;
        f.len[k] = s.end[k] - s.start[k];
    }
}

// key_slice[k]: the slice key k plays, or -1 where the key keeps the kit's own points.
static inline void map(const Analysis& a, const Slices& s, u32 how, int* key_slice) {
    for (int k = 0; k < KEYS; ++k) key_slice[k] = -1;
    const int n = (int)s.n;
    if (n <= 0) return;
    const int wrap = how == ORDER_WRAP || how == KIT_WRAP;
    if (how == KIT || how == KIT_WRAP) {
        Features f;
        features(a, s, f);
        u8 used[MAX_SLICES];
        for (int i = 0; i < n; ++i) used[i] = 0;
        // the median strength: "strong" is at least that
        u32 med = 0;
        {
            u32 cnt_above;
            for (int i = 0; i < n; ++i) {
                cnt_above = 0;
                for (int j = 0; j < n; ++j) if (f.strong[j] >= f.strong[i]) ++cnt_above;
                if (cnt_above * 2 >= (u32)n && f.strong[i] > med) med = f.strong[i];
            }
        }
        const int* order = n >= 6 ? KIT_ORDER6 : KIT_ORDER5;
        const int* role = n >= 6 ? KIT_ROLE6 : KIT_ROLE5;
        u32 kick_bright = 0;
        for (int r = 0; r < 6 && r < n; ++r) {
            int best = -1;
            for (int i = 0; i < n; ++i) {
                if (used[i]) continue;
                if (best < 0) { best = i; continue; }
                if (role[r] == 0) {                             // low: strong ones first, then least bright
                    const int si = f.strong[i] >= med, sb = f.strong[best] >= med;
                    if (si != sb) { if (si) best = i; }
                    else if (f.bright[i] < f.bright[best]) best = i;
                } else if (role[r] == 1) {                      // bright, then short
                    if (f.bright[i] > f.bright[best] || (f.bright[i] == f.bright[best] && f.len[i] < f.len[best])) best = i;
                } else {                                    // the strongest of those clearly brighter than the kicks
                    const int mi = f.bright[i] > 2 * kick_bright, mb = f.bright[best] > 2 * kick_bright;
                    if (mi != mb) { if (mi) best = i; }
                    else if (f.strong[i] > f.strong[best]) best = i;
                }
            }
            used[best] = 1;
            if (role[r] == 0 && f.bright[best] > kick_bright) kick_bright = f.bright[best];
            key_slice[order[r]] = best;
        }
        int next = 0;                                               // the rest in time order onto the free keys
        for (int k = 0; k < KEYS; ++k) {
            if (key_slice[k] >= 0) continue;
            while (next < n && used[next]) ++next;
            if (next >= n) break;
            key_slice[k] = next; used[next] = 1;
        }
        if (wrap) {
            int c = 0;
            for (int k = 0; k < KEYS; ++k) if (key_slice[k] < 0) { key_slice[k] = c; if (++c == n) c = 0; }
        }
        return;
    }
    for (int k = 0; k < KEYS; ++k) {
        if (k < n) key_slice[k] = k;
        else if (wrap) key_slice[k] = k - n * (int)udiv((u32)k, (u32)n);
    }
}

// ---------------------------------------------------------------- the words written

struct Pair { u32 start, end; };

// The Q31 pairs for the keys: a mapped key its slice's, an unmapped one the kit's own (orig). Clamped, end - start
// at least MIN_Q.
static inline void targets(const Slices& s, const int* key_slice, const u32* orig, u32* out, u32 cap = (u32)CAPACITY) {
    for (int k = 0; k < KEYS; ++k) {
        u32 st, en;
        if (key_slice[k] < 0 || (u32)key_slice[k] >= s.n) { st = orig[2 * k]; en = orig[2 * k + 1]; }
        else { st = q31_of_frame(s.start[key_slice[k]], cap); en = q31_of_frame(s.end[key_slice[k]], cap); }
        if (st > 0x7fffffffu - (u32)MIN_Q) st = 0x7fffffffu - (u32)MIN_Q;
        if (en > 0x7fffffffu) en = 0x7fffffffu;
        if (en < st + (u32)MIN_Q) en = st + (u32)MIN_Q;
        out[2 * k] = st; out[2 * k + 1] = en;
    }
}

// The order of the writes: for each key, start first when the new start is below the old end (the pair stays in
// order), end first otherwise. which: 0 start (+4), 1 end (+8). Answers the count (48 at most, unchanged words left out).
struct Write { u8 key, which; u32 value; };
static inline int plan(const u32* old, const u32* now, Write* w) {
    int n = 0;
    for (int k = 0; k < KEYS; ++k) {
        const u32 os = old[2 * k], oe = old[2 * k + 1], ns = now[2 * k], ne = now[2 * k + 1];
        const int start_first = ns <= oe;
        for (int t = 0; t < 2; ++t) {
            const int which = start_first ? t : 1 - t;
            const u32 v = which ? ne : ns, was = which ? oe : os;
            if (v == was) continue;
            w[n].key = (u8)k; w[n].which = (u8)which; w[n].value = v; ++n;
        }
    }
    return n;
}

}  // namespace chop
