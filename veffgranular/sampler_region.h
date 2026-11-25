// sampler_region.h: which frames of a synth sampler slot's pool hold the sample (OS 1.7.3+0). Calls nothing;
// tests/host_test.cpp runs it on a computer.
//
// What is known of the sampler's sample object, from the firmware read on a computer and a probe pack's lines:
//   - object + 0xd768, `frames`: the most frames the sample may hold, 264,600 once a sound was loaded and 264,608 in
//     a slot nothing was ever put in; the pool itself holds 529,200 16-bit words;
//   - byte + 0xd770: 1 for stereo (left and right interleaved);
//   - object + 0xd76c, `words`: written by the LOAD of a sound's file and by nothing else. A drop from the tape and a
//     recording write the pool from its first word and leave `words` (and `frames`) as they were. Version 2 took
//     `words` for the sample's length; that is wrong after any drop or recording, so it is no longer used.
//   - A load writes from the ninth word on and zeroes the rest of the pool. A drop zeroes and writes only the frames
//     it renders, from the first word: what an older, longer sample left behind them stays in the pool.
// So nothing in the object says where a dropped sample ends. The rule here: the sample is the pool up to its last
// frame that is not silent; and when the pack itself made the drop (the drop experiment), it is the frames that drop
// rendered, for as long as the pool is as that drop left it.
#ifndef VEFF_SAMPLER_REGION_H
#define VEFF_SAMPLER_REGION_H

enum { SAMPLER_POOL_WORDS = 529200, SAMPLER_SCAN_STEP = 32 };
enum { SAMPLER_OK = 0, SAMPLER_BAD_STEREO = 4, SAMPLER_EMPTY = 6 };

// the most frames the pool can hold in this layout, and never more than the object says
static inline unsigned sampler_capacity(unsigned frames, unsigned stereo) {
    const unsigned most = stereo ? (unsigned)SAMPLER_POOL_WORDS >> 1 : (unsigned)SAMPLER_POOL_WORDS;
    return frames < most ? frames : most;
}

static inline int sampler_frame_silent(const volatile short* d, unsigned frame, unsigned stereo) {
    return stereo ? d[2u * frame] == 0 && d[2u * frame + 1u] == 0 : d[frame] == 0;
}

// One past the last frame of [0, capacity) that is not silent; 0 when all of it is. It walks back in steps of
// SAMPLER_SCAN_STEP frames while two frames of each step are silent, then frame by frame: a silent pool costs a few
// thousand reads, not half a million. A lone frame of sound inside a silent step can be missed.
static inline unsigned sampler_sounding(const volatile short* d, unsigned capacity, unsigned stereo) {
    unsigned e = capacity;
    while (e >= (unsigned)SAMPLER_SCAN_STEP && sampler_frame_silent(d, e - 1u, stereo) &&
           sampler_frame_silent(d, e - (unsigned)SAMPLER_SCAN_STEP / 2u, stereo)) e -= (unsigned)SAMPLER_SCAN_STEP;
    if (e != capacity) e += (unsigned)SAMPLER_SCAN_STEP;   // the step just passed was only probed: its lower frames may sound
    while (e && sampler_frame_silent(d, e - 1u, stereo)) --e;
    return e;
}

// The region [*first, *end) in frames, or why there is none. `known`: the frames a drop of the pack's own rendered,
// when the pool is still as that drop left it; 0 when not known.
static inline int sampler_region(const volatile short* d, unsigned frames, unsigned stereo, unsigned known, unsigned* first, unsigned* end) {
    *first = *end = 0;
    if (stereo > 1u) return SAMPLER_BAD_STEREO;
    const unsigned capacity = sampler_capacity(frames, stereo);
    unsigned e = known ? (known < capacity ? known : capacity) : sampler_sounding(d, capacity, stereo);
    if (e < 2u) return SAMPLER_EMPTY;
    *end = e;
    return SAMPLER_OK;
}
#endif
