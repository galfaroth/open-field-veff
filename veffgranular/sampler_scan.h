// sampler_scan.h: the search for where a slot's pool stops sounding (sampler_region.h's rule), a little each frame.
// It reads at most `budget` 16-bit words a call and picks up where it stopped, so the frame function never reads
// more than that of a pool in one frame, however large and however silent the pool. Calls nothing;
// tests/host_test.cpp runs it on a computer and holds it to its budget.
#pragma once
#include "sampler_region.h"

namespace sscan {
enum { BUDGET_WORDS = 4096 };                  // a frame's most: a silent stereo pool (33,000 words) takes nine frames
enum { OFF = 0, COARSE = 1, FINE = 2, DONE = 3 };

struct Scan { unsigned state, e, capacity, stereo; };

static inline void begin(Scan& s, unsigned capacity, unsigned stereo) {
    s.state = COARSE; s.e = capacity; s.capacity = capacity; s.stereo = stereo ? 1u : 0u;
}

// One call. Answers the words it read, never more than `budget`. When s.state is DONE, s.e is one past the last
// frame that is not silent (0: the pool is silent), the same answer as sampler_sounding().
static inline unsigned step(Scan& s, const volatile short* d, unsigned budget) {
    unsigned used = 0;
    const unsigned per = s.stereo ? 2u : 1u, pace = (unsigned)SAMPLER_SCAN_STEP;
    if (s.state == COARSE) {
        for (;;) {
            if (s.e < pace) { s.state = FINE; break; }
            if (used + 2u * per > budget) return used;                 // the next two probes would not fit: next frame
            used += 2u * per;
            if (!(sampler_frame_silent(d, s.e - 1u, s.stereo) && sampler_frame_silent(d, s.e - pace / 2u, s.stereo))) { s.state = FINE; break; }
            s.e -= pace;
        }
        if (s.e != s.capacity) s.e += pace;    // the step just passed was only probed: its lower frames may sound
    }
    if (s.state == FINE) {
        for (;;) {
            if (!s.e) { s.state = DONE; break; }
            if (used + per > budget) return used;
            used += per;
            if (!sampler_frame_silent(d, s.e - 1u, s.stereo)) { s.state = DONE; break; }
            --s.e;
        }
    }
    return used;
}
}  // namespace sscan
