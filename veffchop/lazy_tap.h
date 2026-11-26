// lazy_tap.h: veffchop 7's LAZY, as on the OP-1 and the OP-XY: the sample plays through on the lowest key, and every
// tap marks a cut where the sound is; each slice runs to the next tap. Plain code, apart from the unit: the taps, the
// slices made of them, and which voice plays the lowest key and where. veffchop.cpp reads the unit's words and calls
// these; tests/host_test.cpp runs them on a computer. Include after veffchop_engine.h (Slices, MAX_SLICES, frames_of_ms).
//
// The drum voices (veffdrumprobe FINDINGS sections 5 and 6, and the v2 probe's log): eight voices at 0x119062d8 + 0x38 i;
// the key a voice plays, as an index 0..23, at 0x11903560 + 4 i; the voice's play position at voice + 0x18, a frame
// index with 11 bits behind the point (20.11: the render multiplies the key's Q31 start and end by capacity << 11). In
// the probe's log a voice on key 5 of a kit cut in 24 ran from 0x1196efff to 0x157ef7ff, frames 144,093 to 176,094,
// where key 5's points lie; the word at voice + 0x2c was 1 or 2 while it sounded and 0 once it had ended.
#pragma once

namespace lazy {
typedef unsigned u32;

enum { VOICES = 8, VOICE_BYTES = 0x38, POS_AT = 0x18, STATE_AT = 0x2c, CUTS_MOST = 23, GAP_MS = 20 };

// The frame a voice's position word stands for.
static inline u32 frame_of_position(u32 pos) { return pos >> 11; }

// The voice that plays key `key` (0..23) and sounds: its number, or -1. Of several, the one furthest on.
static inline int playing(const u32* keys, const u32* states, const u32* positions, u32 key) {
    int best = -1;
    for (u32 v = 0; v < (u32)VOICES; ++v) {
        if (keys[v] != key || states[v] == 0) continue;
        if (best < 0 || positions[v] > positions[best]) best = (int)v;
    }
    return best;
}

struct Taps {
    u32 n;
    u32 at[CUTS_MOST];                  // frames, ascending
};

static inline void clear(Taps& t) { t.n = 0; }

// A tap at frame f of a sample of `frames`: taken when it lies at least `gap` frames from the start, from the end and
// from every cut already there, and fewer than CUTS_MOST are there. Answers 1 when taken.
static inline int add(Taps& t, u32 f, u32 frames, u32 gap) {
    if (t.n >= (u32)CUTS_MOST || f < gap || f + gap > frames) return 0;
    u32 i = 0;
    while (i < t.n && t.at[i] < f) ++i;
    if (i > 0 && f - t.at[i - 1] < gap) return 0;
    if (i < t.n && t.at[i] - f < gap) return 0;
    for (u32 k = t.n; k > i; --k) t.at[k] = t.at[k - 1];
    t.at[i] = f;
    ++t.n;
    return 1;
}

// The slices: the first from frame 0, then one from each tap, each to the frame before the next (the last to the
// sample's last frame).
static inline void slices(const Taps& t, u32 frames, chop::Slices& s) {
    s.n = t.n + 1u;
    s.start[0] = 0;
    for (u32 i = 0; i < t.n; ++i) s.start[i + 1u] = t.at[i];
    for (u32 i = 0; i < s.n; ++i) s.end[i] = i + 1u < s.n ? s.start[i + 1u] - 1u : (frames ? frames - 1u : 0u);
}
}  // namespace lazy
