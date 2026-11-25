// grain_source.h: the one thing the grain DSP knows about where its sample comes from.
//
// The DSP (granular_core.h) reads a sample only through a GrainSource, a plain description of 16-bit frames that
// somebody else owns. Nothing else in the DSP knows whether the frames came from a file in the pack's folder
// (today's source, veffgranular.cpp, "the file source") or from the unit's own synth sampler (the later source, see
// "THE SAMPLER SOURCE" in veffgranular.cpp).
//
// Lifetime rules (the contract both sides keep):
//   1. The render reads the description ONCE, at its start (granular_render takes a copy), and uses only that copy
//      until it returns. It never keeps `data` past the call.
//   2. The owner (the frame function, on the display thread) never changes the frames or any field while `valid` is
//      not 0. To change them it first writes `valid` = 0 (one word), then changes data and fields, then writes a new
//      `generation` and only then `valid` = 1. On the unit the audio thread runs ahead of the display thread and a
//      render is never interrupted by a frame function, so a render either sees the old complete description or 0.
//   3. When the render sees a `generation` other than the one its grains were started on, it drops every grain
//      (their positions belong to other frames).
//   4. `data` holds `frames * (stereo ? 2 : 1)` 16-bit words, left first when stereo. 0 <= start < end <= frames.
//      The DSP reads only frames in [start, end - 1] and never past them, whatever its values are.
#pragma once

struct GrainSource {
    const short* data;      // the frames, interleaved when stereo
    unsigned frames;        // frames in data (at most GRAIN_SOURCE_FRAMES_MOST)
    unsigned stereo;        // 0 mono, 1 stereo
    unsigned start, end;    // the region grains may read: [start, end)
    unsigned root;          // the MIDI note that plays the sample at its own pitch (60 = C4)
    unsigned rate_q16;      // the sample's rate over 44,100, Q16 (65536 for 44.1 kHz)
    unsigned generation;    // changes whenever the frames change
    unsigned valid;         // 0: nothing to play (the render is silent)
};

enum { GRAIN_SOURCE_FRAMES_MOST = 1u << 22 };   // positions are kept in whole frames below 2^22

// A source the DSP will play: the checks the render does before touching a frame.
static inline int grain_source_ok(const GrainSource& s) {
    return s.valid && s.data && s.frames >= 2u && s.frames <= (unsigned)GRAIN_SOURCE_FRAMES_MOST && s.end <= s.frames &&
           s.start < s.end && s.end - s.start >= 2u && s.rate_q16 >= 8192u && s.rate_q16 <= 4u * 65536u && s.root < 128u;
}
