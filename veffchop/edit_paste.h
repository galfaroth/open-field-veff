// edit_paste.h: veffchop 8's key EDIT and COPY/PASTE, the decisions apart from the unit: the zoomed view, a knob's step,
// a key's start and end held in order, where a pasted sound goes in the slot's memory and whether it fits, and the
// conversion of a copied sound between mono and stereo. It calls nothing and reads nothing of the unit's;
// veffchop.cpp gathers the words and does the reads and writes, tests/host_test.cpp runs this code on a computer.
#pragma once

namespace edit {
typedef unsigned u32;
typedef short i16;

enum {
    ZOOMS = 13,                     // zoom 0 the whole span, each step half the one before
    SPAN_LEAST = 220,               // 5 ms: the narrowest view
    POOL_BYTES = 0x35d540,          // a drum slot's pool: 882000 stereo frames
    PAD_BYTES = 16,                 // the pool's head before the first frame
    PASTE_GAP = 64,                 // frames left between the slot's material and a pasted sound
    CLIP_BYTES = 1764000,           // the pack's memory for a copied sound: 10 s stereo (441,000 frames), 20 s mono
    LEAST_LENGTH = 432              // frames between a key's start and end at least (MIN_Q of 882,000, rounded up)
};

// The capacity a key's fractions are of: 882,000 stereo, 529,200 mono.
static inline u32 capacity(u32 stereo) { return stereo ? 882000u : 529200u; }

// The frames a view shows at a zoom, of a whole of `total` frames.
static inline u32 span(u32 total, u32 zoom) {
    u32 s = total >> (zoom < (u32)ZOOMS ? zoom : (u32)ZOOMS - 1u);
    return s < (u32)SPAN_LEAST ? (total < (u32)SPAN_LEAST ? total : (u32)SPAN_LEAST) : s;
}

// The view's first frame: `centre` in its middle, held inside 0..total.
static inline u32 view_first(u32 total, u32 sp, u32 centre) {
    if (sp >= total) return 0;
    u32 f = centre > sp / 2u ? centre - sp / 2u : 0u;
    return f + sp > total ? total - sp : f;
}

// One step of a knob in frames: a column of the view (cols columns), at least 1.
static inline u32 step(u32 sp, u32 cols) {
    u32 s = 0;
    while ((s + 1u) * cols <= sp) ++s;              // sp / cols without a division (cols small, called once a turn)
    return s ? s : 1u;
}
// A faster form for the unit: the same as step(), by shifts when cols is a power of two, else the loop above.

// A frame moved by `steps` (signed) of `st` frames, held to 0..most.
static inline u32 moved(u32 f, int steps, u32 st, u32 most) {   // no 64-bit product: the unit links no runtime
    u32 n = (u32)(steps < 0 ? -steps : steps);
    if (n > 4096u) n = 4096u;
    if (st > 0x40000u) st = 0x40000u;                              // n x st stays under 2^30
    const u32 d = n * st;
    u32 v = steps < 0 ? (d > f ? 0u : f - d) : (f + d < f ? 0xffffffffu : f + d);
    return v > most ? most : v;
}

// A key's start and end held in order: end at least LEAST_LENGTH after start, both inside 0..most. `moving_end` says
// which the owner moved: the other one stays where it is when it can.
static inline void in_order(u32& start, u32& end, u32 most, int moving_end) {
    if (start > most) start = most;
    if (end > most) end = most;
    if (end < start + (u32)LEAST_LENGTH) {
        if (moving_end || start + (u32)LEAST_LENGTH > most) {
            if (moving_end) end = start + (u32)LEAST_LENGTH <= most ? start + (u32)LEAST_LENGTH : most;
            if (end < start + (u32)LEAST_LENGTH) start = end >= (u32)LEAST_LENGTH ? end - (u32)LEAST_LENGTH : 0u;
        } else {
            start = end >= (u32)LEAST_LENGTH ? end - (u32)LEAST_LENGTH : 0u;
        }
    }
}

// Where a pasted sound of `n` frames goes in a slot whose material ends at frame `used`: behind it, on an even frame
// (a mono frame is a halfword; two make a word). Answers 1 with `at`, or 0 with `missing`, the frames it lacks: the
// slot's capacity at its channels, and its pool's bytes, both held.
static inline int place(u32 used, u32 n, u32 stereo, u32& at, u32& missing) {
    u32 a = used + (u32)PASTE_GAP;
    a = (a + 1u) & ~1u;
    const u32 cap = capacity(stereo);
    const u32 room_bytes = (u32)POOL_BYTES - (u32)PAD_BYTES;
    const u32 pool_frames = stereo ? room_bytes / 4u : room_bytes / 2u;
    const u32 most = cap < pool_frames ? cap : pool_frames;
    at = a;
    if (a <= most && n <= most - a) { missing = 0; return 1; }
    missing = a + n - most;
    return 0;
}

// The end of the slot's material: the sample's own length (its words, or the last drop's), the furthest key's end, and
// the end of what veffchop pasted there itself; the greatest.
static inline u32 used(u32 material, const u32* key_ends, u32 keys, u32 pasted) {
    u32 u = material > pasted ? material : pasted;
    for (u32 k = 0; k < keys; ++k) if (key_ends[k] + 1u > u) u = key_ends[k] + 1u;
    return u;
}

// One frame of a copied sound (`from_stereo` its channels, halfwords L,R or one) as the slot takes it (`to_stereo`):
// stereo to mono the two averaged, mono to stereo the one twice.
static inline void convert(const i16* in, u32 from_stereo, u32 to_stereo, i16* out) {
    if (from_stereo == to_stereo) { out[0] = in[0]; if (to_stereo) out[1] = in[1]; return; }
    if (from_stereo) { out[0] = (i16)(((int)in[0] + (int)in[1]) >> 1); return; }
    out[0] = in[0]; out[1] = in[0];
}

// The frames a copied sound may hold at its channels in the pack's memory.
static inline u32 clip_room(u32 stereo) { return stereo ? (u32)CLIP_BYTES / 4u : (u32)CLIP_BYTES / 2u; }
}  // namespace edit
