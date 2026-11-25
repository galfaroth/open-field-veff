// sampler_region.h: which frames of a synth sampler slot's pool hold the sample (OS 1.7.3+0), from the three fields
// the sampler's object keeps. Calls nothing; tests/host_test.cpp runs it on a computer.
//
// What a probe pack printed on one unit (three starts):
//   - `frames` (object + 0xd768) is the capacity the sampler set: 264,600 for a slot with a sample, mono or stereo,
//     and 264,608 for a slot nothing was ever put in;
//   - `words` (object + 0xd76c) is how many 16-bit words the last load, drop or recording wrote, counted from the
//     start of the data, the 8 words of zero head pad included: 16,392 after a drop of 16,384 mono frames, 529,200 for
//     a full stereo sample; 0 for an empty slot, and 0 for a moment while a load is under way;
//   - the byte at + 0xd770 is 1 for stereo (left and right interleaved).
// So the sample is the words [8, words) of the data: mono frames [8, words), stereo frames [4, words / 2), never past
// `frames` nor the pool.
#ifndef VEFF_SAMPLER_REGION_H
#define VEFF_SAMPLER_REGION_H

enum { SAMPLER_POOL_WORDS = 529200, SAMPLER_HEAD_WORDS = 8 };
enum { SAMPLER_OK = 0, SAMPLER_EMPTY = 6, SAMPLER_BAD_STEREO = 4 };

// Answers SAMPLER_OK and the region [*first, *end) in frames, or why there is none.
static inline int sampler_region(unsigned frames, unsigned words, unsigned stereo, unsigned* first, unsigned* end) {
    *first = *end = 0;
    if (stereo > 1u) return SAMPLER_BAD_STEREO;
    if (words > (unsigned)SAMPLER_POOL_WORDS) words = (unsigned)SAMPLER_POOL_WORDS;       // never past the pool
    unsigned f = stereo ? (unsigned)SAMPLER_HEAD_WORDS >> 1 : (unsigned)SAMPLER_HEAD_WORDS;
    unsigned e = stereo ? words >> 1 : words;
    if (e > frames) e = frames;                                                           // nor past what the object says it holds
    if (e < f + 2u) return SAMPLER_EMPTY;                                                 // nothing, or under two frames
    *first = f; *end = e;
    return SAMPLER_OK;
}
#endif
