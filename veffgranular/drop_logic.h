// drop_logic.h: the decisions of the drop experiment (DROP.md), apart from the unit: which key is taken, which
// checks must hold, and the few frames from the key to the call. It calls nothing; veffgranular.cpp gathers the
// facts and makes the one call, and tests/host_test.cpp runs this very code on a computer.
#pragma once

namespace dropx {
typedef unsigned u32;

enum { KEY_DEFAULT = 11 };       // the drop key's number as a card sees it: a reading, not confirmed (DROP.md)
enum { FADE_FRAMES = 3 };        // frames between silencing the slot and the call
enum { MESSAGE_FRAMES = 90 };

// why a drop is refused; the page shows the number
enum { OK = 0, R_SITE = 1, R_NOT_OURS = 2, R_DISK = 3, R_CHAIN = 4, R_NOTHING = 5, R_TRACKS = 6, R_WORKER = 7 };

// what the frame function read, this frame
struct Facts {
    u32 site_ok;                 // the firmware's bytes at the call site sum to what OS 1.7.3+0's do
    u32 ours;                    // our page is up for a synth slot 0..7 whose sound rides our engine
    u32 disk;                    // the disk mode is open
    u32 chain_ok;                // the slot's table word, holder, object and pool pass the sampler source's checks
    u32 clip_length, clip_other; // the clipboard's two words: both 0 is an empty clipboard, as stock's own check reads
    u32 clip_tracks;             // how many tracks the clipboard holds
    u32 queue_read, queue_write; // the worker's queue: equal when no request waits or runs
};

static inline int guard(const Facts& f) {
    if (!f.site_ok) return R_SITE;
    if (!f.ours) return R_NOT_OURS;
    if (f.disk) return R_DISK;
    if (!f.chain_ok) return R_CHAIN;
    if (!f.clip_length && !f.clip_other) return R_NOTHING;
    if (f.clip_tracks != 1u) return R_TRACKS;
    if (f.queue_read != f.queue_write) return R_WORKER;
    return OK;
}

enum { IDLE = 0, FADING = 1 };
enum { M_NONE = 0, M_DROPPING = 1, M_DROPPED = 2, M_NOTHING = 3, M_REFUSED = 4 };
enum { A_NONE = 0, A_HOLD = 1, A_CALL = 2, A_RELEASE = 3 };

struct Machine {
    u32 state, slot, frames;
    u32 message, message_frames; int reason;
    u32 done, refused;
};

static inline void refuse(Machine& m, int why) {
    ++m.refused; m.reason = why;
    m.message = why == R_NOTHING ? (u32)M_NOTHING : (u32)M_REFUSED; m.message_frames = MESSAGE_FRAMES;
    m.state = IDLE;
}

// One frame. `request`: 0, or the slot + 1 the drop key asked for since the last frame. `f` belongs to the slot the
// request names while idle, and to m.slot while fading. Answers what the caller must do now:
//   A_HOLD     silence m.slot (its source not valid) from this frame on
//   A_CALL     make the call for m.slot, then call called(); the checks held a second time, this very frame
//   A_RELEASE  let m.slot sound again; the drop was refused after the hold
static inline int step(Machine& m, u32 request, const Facts& f) {
    if (m.message_frames && --m.message_frames == 0) m.message = M_NONE;
    if (m.state == IDLE) {
        if (!request || request > 8u) return A_NONE;
        const int why = guard(f);
        if (why) { refuse(m, why); return A_NONE; }
        m.slot = request - 1u; m.state = FADING; m.frames = 0;
        m.message = M_DROPPING; m.message_frames = 0;       // stays until the call is over
        return A_HOLD;
    }
    if (++m.frames < (u32)FADE_FRAMES) return A_NONE;        // a request while fading is not looked at
    const int why = guard(f);
    if (why) { refuse(m, why); return A_RELEASE; }
    return A_CALL;
}
static inline void called(Machine& m) {
    ++m.done; m.state = IDLE; m.message = M_DROPPED; m.message_frames = MESSAGE_FRAMES;
}

// A key that went down. 1: taken, and it asks for a drop. 2: taken and nothing more (the keys beside the drop key
// while a drop of ours is under way, so that stock starts no lift or drop of its own meanwhile). 0: not ours.
static inline int key_down(u32 key, u32 drop_key, u32 shift, u32 mine, u32 armed, u32 busy) {
    if (!mine || !armed) return 0;
    if (busy && (key == drop_key || (key >= 10u && key <= 12u))) return 2;
    if (key != drop_key || shift) return 0;                  // shift and drop is stock's mix drop: left to stock
    return 1;
}

// the key's number from the text of dropkey.txt: digits, then anything. Only a number a key outside the keyboard can
// have (0..12, 54..127) is taken; anything else answers the default.
static inline u32 key_from_text(const char* t, u32 bytes) {
    u32 v = 0, any = 0, i = 0;
    while (i < bytes && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    for (; i < bytes && t[i] >= '0' && t[i] <= '9' && v < 1000u; ++i) { v = v * 10u + (u32)(t[i] - '0'); any = 1; }
    if (!any || v > 127u || (v >= 13u && v <= 53u)) return KEY_DEFAULT;
    return v;
}
}  // namespace dropx
