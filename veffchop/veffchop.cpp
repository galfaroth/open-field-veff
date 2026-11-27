// veffchop: chops the current drum kit's sample into the 24 drum keys, for OS 1.7.3 (1.7.3+0) only. Version 6: mono
// samples too, and the drop key on the page through the base's op1f_sample_drop (DROP.md). Version 4: a drum
// engine (base 43, OP1F_ASK_DRUM_ENGINE, docs/drum-engine.md): 'veffchop' stands in the drum's engine list, rides the
// drum sampler, and once picked on a drum sound its page stands on T1 in the drum's mode; it plays nothing itself and
// lets every keyboard key through to the kit. veffdrumprobe/FINDINGS.md (sections 6 and 7) says where every address comes from.
//
//   CUR_SLOT   [0x84a01528]: the drum slot being edited, 0..7
//   HOLDERS    0x0802f0ac + 4 s: the slot's holder; [holder] the object; [object] the data (int16 L,R from data + 16),
//              [object + 0xd76c] words (2 frames + 8 for stereo), byte [object + 0xd770] stereo
//   KEY_TABLE  0x84a03cc8 + s * 0x300 + k * 0x20: key k's record; +4 start, +8 end, Q31 fractions of 882000 frames
//
// The pack writes only +4 and +8 of the current slot's 24 records, only on a press of the fourth knob, all in one
// frame, in an order that never leaves a start past its end. It keeps the kit's own 48 words first, so that shift
// and the fourth knob's press puts them back. Everything else of the kit (pitch, mode, pan, volume) is never touched.
//
// The sample is read once (when its words changed and then stayed put for half a second), on the display thread,
// at most chop::BUDGET words a frame (veffchop_engine.h); the knobs only filter what that reading found.
//
// Threads: input (the key thread for knobs, maybe the audio thread for the keyboard's keys 13 to 53) counts turns and
// presses and notes the key struck, each in single words, and lets the keyboard's keys through; the frame function
// does all the rest. Version 2 also builds the candidates over frames (finish_step, FINISH_BUDGET windows a frame).

#include "build_r6e_card.h"
#include "veff_ui.h"
#include "veffchop_engine.h"
#include "lazy_tap.h"
#include "edit_paste.h"

typedef unsigned u32;
typedef unsigned char u8;
using namespace chop;

static const char LOG_FILE[] = "op1f/veffchop.cpp";
enum { ASK = 0, SLOTS = 8, STEADY_MS = 500, NOTE_MS = 2500,
       OFF_FRAMES = 0xd768, OFF_WORDS = 0xd76c, OFF_STEREO = 0xd770, OBJECT_BYTES = 0xd774, KEY_BYTES = 0x20, KIT_BYTES = 0x300, DATA_PAD = 16 };
static const u32 VOICE0 = 0x119062d8u, VOICE_KEYS = 0x11903560u;   // v7: the drum voices, and the key index each plays
static const u32 HOLDERS = 0x0802f0acu, CUR_SLOT = 0x84a01528u, KEY_TABLE = 0x84a03cc8u;
static const u32 DDR_LO = 0x80000000u, DDR_HI = 0x90000000u, SDRAM_LO = 0x08000000u, SDRAM_HI = 0x08100000u;

extern "C" const Op1fCard op1f_card_veffchop OP1F_NAMED("op1f_card_veffchop");

// ---------------------------------------------------------------- the unit's memory
#ifdef VEFFCHOP_HOST
u32 host_rd32(u32 a);
u32 host_rd8(u32 a);
void host_wr32(u32 a, u32 v);
static inline u32 rd32(u32 a) { return host_rd32(a); }
static inline u32 rd8(u32 a) { return host_rd8(a); }
static inline void wr32(u32 a, u32 v) { host_wr32(a, v); }
#else
static inline u32 rd32(u32 a) { return *(const volatile u32*)a; }
static inline u32 rd8(u32 a) { return *(const volatile u8*)a; }
static inline void wr32(u32 a, u32 v) { *(volatile u32*)a = v; }
#endif
#ifdef VEFFCHOP_HOST
void host_pool_wr32(u32 a, u32 v);
static inline void pool_wr32(u32 a, u32 v) { host_pool_wr32(a, v); }
#else
static inline void pool_wr32(u32 a, u32 v) { *(volatile u32*)a = v; }   // v8: a word of the slot's pool, behind its material
#endif
static inline int in(u32 a, u32 bytes, u32 lo, u32 hi) { return !(a & 3) && a >= lo && a < hi && bytes <= hi - a; }
static u32 words_read;                                   // sample words read this frame (the budget, for the tests)
static u32 sample_rd(u32 a) { ++words_read; return rd32(a); }


// The current slot's sample, every address checked. Answers 0 when it is usable, or why not.
enum Why { OK = 0, NO_SLOT, NO_OBJECT, NO_SAMPLE, MONO, BAD_STEREO, TOO_LONG };
static const char* const WHY_TEXT[] = { "", "NO DRUM SLOT", "NO SAMPLE OBJECT", "NO SAMPLE", "MONO: NOT IN V1", "UNKNOWN KIND", "TOO LONG" };
struct Where { u32 slot, obj, data, words, stereo, frames, table; };
// v6: after a drop the firmware's words still say the sample loaded before (the import writes neither the words nor
// the frames): the base's `dropped` (op1f_sample_get) is the length, until a sound is loaded there.
static u32 dropped_now[8];
static int locate(Where& w) {
    w.slot = w.obj = w.data = w.words = w.stereo = w.frames = w.table = 0;
    if (!in(CUR_SLOT, 4, DDR_LO, DDR_HI)) return NO_SLOT;
    w.slot = rd32(CUR_SLOT);
    if (w.slot >= (u32)SLOTS) return NO_SLOT;
    w.table = KEY_TABLE + w.slot * KIT_BYTES;
    if (!in(w.table, KIT_BYTES, DDR_LO, DDR_HI)) return NO_SLOT;
    const u32 h = rd32(HOLDERS + w.slot * 4);
    if (!in(h, 4, DDR_LO, DDR_HI)) return NO_OBJECT;
    w.obj = rd32(h);
    if (!in(w.obj, OBJECT_BYTES, DDR_LO, DDR_HI)) return NO_OBJECT;
    w.words = rd32(w.obj + OFF_WORDS);
    // v6: the sample's address and channels as the base hands them (it holds the object to the slot's own pool)
    Op1fSample got;
    if (op1f_sample_get(&op1f_card_veffchop, (u32)OP1F_PLACE_DRUM + w.slot, &got) != 0 || got.data == 0) return NO_OBJECT;
    w.data = (u32)(unsigned long)got.data;
    w.stereo = got.stereo;
    dropped_now[w.slot] = got.dropped;
    if (w.stereo > 1) return BAD_STEREO;
    // v6: mono too. The render's capacity is 882000 frames with the stereo byte set and 529200 without (0x8f0137c0),
    // a mono sample a halfword a frame; the words are the frames (twice them for stereo) and 8.
    if (w.words < 8u + (w.stereo ? 2u : 1u) * 4410u) return NO_SAMPLE;  // under 0.1 s: nothing to chop
    w.frames = w.stereo ? (w.words - 8u) >> 1 : w.words - 8u;
    if (dropped_now[w.slot]) w.frames = dropped_now[w.slot];          // v6: a drop's length, while no sound was loaded since
    if (w.frames > capacity_of(w.stereo)) return TOO_LONG;
    if (!in(w.data, DATA_PAD + w.frames * (w.stereo ? 4u : 2u), DDR_LO, DDR_HI)) return NO_SAMPLE;
    return OK;
}

// ---------------------------------------------------------------- what the other threads hand over (single words)
static volatile u32 shown_where, shown_how;
static volatile u32 struck;                              // (count << 8) | the keyboard key's number, of the last key down
static volatile int turned[4], turned_shift[4];          // turns, 1024 a step, summed
static volatile u32 pressed[4], pressed_shift[4];        // presses counted
static volatile u32 shift_held;                          // v3: shift (key 38) down, seen by its own down and up
static volatile u32 press4_how;                          // v3: (count << 8) | how the last press of the 4th knob saw shift
static volatile u32 lazy_armed, lazy_playing;           // v7: written by the frame, read by the input function
static volatile u32 tapped;                              // v7: taps counted by the input function (keys while armed and playing)
static volatile u32 drop_down;                           // v6: the place the drop key went down on, 0 none
static volatile int drop_answer;                         // v6: op1f_sample_drop's answer at the key's up
static volatile u32 drop_asked;                          // v6: (count << 8) | the place, written after drop_answer
enum { DROP_KEY = 11 };
enum { DM_NONE = 0, DM_DROPPED = 1, DM_REFUSED = 2 };
static u32 drop_message, drop_message_at;                // the frame's own
static int drop_reason;
static u32 drop_ticket;                                  // v6: the ticket waited for, 0 none

// The drum engine's page is up: shown(OP1F_PLACE_DRUM + slot, OP1F_SHOWN_UP).
static inline int page_up() {
    const u32 w = shown_where;
    return (shown_how & (u32)OP1F_SHOWN_UP) && w >= (u32)OP1F_PLACE_DRUM && w < (u32)OP1F_PLACE_DRUM + (u32)SLOTS;
}

extern "C" void veffchop_shown(unsigned where, unsigned how) OP1F_NAMED("veffchop_shown");
extern "C" void veffchop_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

// The keyboard's keys (13 to 53 but shift, 38) are let through so the kit plays them; their number is noted for the
// highlight. They may come on the audio thread: one word written, nothing else. The knobs and their clicks are taken.
enum { KEYBOARD_LO = 13, KEYBOARD_HI = 53, SHIFT_KEY = 38, LAZY_KEY = 39 };   // v7: 39, the lowest F: drum key 1
extern "C" int veffchop_input(const Op1fInput* e) OP1F_NAMED("veffchop_input");
extern "C" int veffchop_input(const Op1fInput* e) {
    if (e->bytes < 36u || !e->mine || !page_up()) return 0;
    if (e->what != (u32)OP1F_TURN && e->key == (u32)SHIFT_KEY) {    // v3: shift tracked by its own down and up
        if (e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) shift_held = 1;
        else if (e->what == (u32)OP1F_UP) shift_held = 0;
        return 0;                                        // the unit sees it as always
    }
    if (e->what != (u32)OP1F_TURN && e->key >= (u32)KEYBOARD_LO && e->key <= (u32)KEYBOARD_HI && e->key != (u32)LAZY_KEY &&
        lazy_armed && lazy_playing) {                    // v7: a tap while the lowest key plays the sample: taken, silent
        if (e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) tapped = tapped + 1u;
        return 1;
    }
    if (e->what != (u32)OP1F_TURN && e->key >= (u32)KEYBOARD_LO && e->key <= (u32)KEYBOARD_HI) {
        if (e->key != (u32)SHIFT_KEY && (e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT))
            struck = (((struck >> 8) + 1u) << 8) | (e->key & 0xffu);
        return 0;                                        // the kit plays it
    }
    const int shift = e->shift || e->what == (u32)OP1F_DOWN_SHIFT || shift_held;
    if (e->what != (u32)OP1F_TURN && e->key == (u32)DROP_KEY) {                // v6: the drop key, as docs/sample.md has it
        if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && !shift) {
            drop_down = shown_where;                     // taken on its down (shift and drop stays the unit's)
            return 1;
        }
        if (e->what != (u32)OP1F_UP || !drop_down) return 0;   // an up comes for a down we took
        const u32 place = drop_down;
        drop_down = 0;
        drop_answer = op1f_sample_drop(&op1f_card_veffchop, ASK, place);   // the ask: our drum engine, ask 0
        drop_asked = (((drop_asked >> 8) + 1u) << 8) | (place & 0xffu);    // the answer first, then the word the frame watches
        return 1;
    }
    if (e->what == (u32)OP1F_TURN) {
        if (e->key > 3u) return 0;
        if (shift) turned_shift[e->key] = turned_shift[e->key] + e->turn;
        else turned[e->key] = turned[e->key] + e->turn;
        return 1;
    }
    if (e->key >= 54u && e->key <= 57u) {
        if (e->what == (u32)OP1F_UP) return 1;
        if (e->what != (u32)OP1F_DOWN && e->what != (u32)OP1F_DOWN_SHIFT) return 0;
        if (e->key == 57u)
            press4_how = (((press4_how >> 8) + 1u) << 8) | (e->shift ? 1u : 0u) | (e->what == (u32)OP1F_DOWN_SHIFT ? 2u : 0u) | (shift_held ? 4u : 0u);
        if (shift) pressed_shift[e->key - 54u] = pressed_shift[e->key - 54u] + 1u;
        else pressed[e->key - 54u] = pressed[e->key - 54u] + 1u;
        return 1;
    }
    return 0;
}

// The drum key (0..23) of a keyboard key's number, as struck on the owner's unit (v2's line 9, the 24 keys from the
// lowest F to the highest E). The firmware's numbers follow no one range: the white keys of the lower F..B are 39-42,
// of C..F in the middle 28-31, of the upper G..E 48-53; the black keys 13-15 (F# G# A# low), 32 (C# low), 22 (D# low),
// 23-27 (F# G# A# C# D# high). -1 for any other number.
static const u8 KEY_NUMBER[KEYS] = {
    39, 13, 40, 14, 41, 15, 42,       // F  F# G  G# A  A# B
    28, 32, 29, 22, 30,               // C  C# D  D# E
    31, 23, 48, 24, 49, 25, 50,       // F  F# G  G# A  A# B
    51, 26, 52, 27, 53,               // C  C# D  D# E
};
static int drum_key_of(u32 n) {
    for (int k = 0; k < KEYS; ++k) if (KEY_NUMBER[k] == n) return k;
    return -1;
}

// ---------------------------------------------------------------- the frame function's own
enum State { WAITING = 0, STEADYING, ANALYSING, READY, REFUSED, FINISHING };
// v9: the analysis (some 55 KB) lives in the memory ask behind the copy's room, not in the pack's zeroed data, which
// counts against the room for pack files on the unit. Set at the first frame the memory is there; nothing reads it
// before (the frame function waits for it).
enum { ANALYSIS_ROOM = 57344 };
static_assert(sizeof(Analysis) <= (unsigned)ANALYSIS_ROOM, "the analysis fits its room in the memory ask");
static Analysis* analysis;
#define an (*analysis)
static Slices sl;
static int key_slice[KEYS];
static struct {
    u32 started, stood, state, why, why_logged;
    Where at;                                            // the sample analysed (or being steadied)
    u32 seen_words, seen_since, seen_checks;
    u32 analyse_frames;
    Params p;
    u32 dirty;
    int turn_seen[4], turn_shift_seen[4], turn_rest[4], turn_shift_rest[4];
    u32 press_seen[4], press_shift_seen[4];
    u32 note, note_at;                                   // a word on the page for a moment: APPLIED, UNDONE, ...
    u32 struck_seen, struck_key, struck_at, struck_valid, keys_logged[2];
    u32 shown_where_seen, shown_how_seen, press4_seen;
    // the kit's own pairs, kept at the first apply to a slot's sample: undo puts them back
    u32 orig_words[SLOTS], orig_valid[SLOTS];
    u32 orig[SLOTS][2 * KEYS];
    u32 written[SLOTS][2 * KEYS], written_valid[SLOTS];
    u32 applied;
    // v7: LAZY
    u32 armed, armed_slot, tapped_seen, play_frame, playing, taps_logged;
    lazy::Taps taps;
    // v8: EDIT of one key, and COPY/PASTE
    u32 editing, ed_slot, ed_key, ed_start, ed_end, ed_zoom, ed_centre, ed_total, ed_stereo, ed_data, ed_moved;
    u32 zc_first, zc_span, zc_next, zc_slot;               // the zoomed wave's span and the next column to read
    u16 zc[COLS];
    u32 ed_playing, ed_play_frame;
    u32 clip_frames, clip_stereo, copy_state, copy_at, copy_from, copy_n;
    u32 paste_state, paste_slot, paste_key, paste_at, paste_n, paste_done, paste_stereo, paste_data, paste_ticket, paste_ms;
    u32 paste_end[SLOTS], paste_obj[SLOTS], paste_words[SLOTS];
    u32 ed_note, ed_note_at, ed_missing, ed_logged;
} st;
static u32 target[2 * KEYS], current[2 * KEYS];
static Write writes[2 * KEYS];

enum Note { N_NONE = 0, N_APPLIED, N_UNDONE, N_NOTHING_TO_UNDO, N_NOT_READY, N_CHANGED };
static const char* const NOTE_TEXT[] = { "", "APPLIED", "UNDONE", "NOTHING TO UNDO", "NOT READY", "SAMPLE CHANGED" };
static const char* const MODE_TEXT[MODES] = { "TRANSIENT", "LAZY", "EQUAL" };
static const char* const MAP_TEXT[MAPS] = { "ORDER", "WRAP", "KIT", "KIT+WRAP" };

static void defaults() {
    st.p.mode = TRANSIENT; st.p.sens = 50; st.p.parts = 16; st.p.map = ORDER_WRAP; st.p.gap_ms = 100; st.p.offset_ms = 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Turns since the last frame, in steps (1024 a step), the remainder kept.
static int steps_of(volatile int& total, int& seen, int& rest) {
    const int now = total, d = now - seen;
    seen = now;
    rest += d;
    const int s = rest >= 0 ? (rest >> 10) : -((-rest) >> 10);
    rest -= s * 1024;
    return s;
}

static void edit_knobs();
static void knobs() {
    if (st.editing) { edit_knobs(); return; }                                       // v8
    for (int i = 0; i < 4; ++i) {
        const int s = steps_of(turned[i], st.turn_seen[i], st.turn_rest[i]);
        const int ss = steps_of(turned_shift[i], st.turn_shift_seen[i], st.turn_shift_rest[i]);
        if (s) {
            if (i == 0) st.p.mode = (u32)clampi((int)st.p.mode + s, 0, MODES - 1);
            else if (i == 1) {
                if (st.p.mode == EQUAL) st.p.parts = (u32)clampi((int)st.p.parts + s, 2, 24);
                else st.p.sens = (u32)clampi((int)st.p.sens + s, 0, 100);
            } else if (i == 2) st.p.map = (u32)clampi((int)st.p.map + s, 0, MAPS - 1);
            st.dirty = 1;
        }
        if (ss) {
            if (i == 0) st.p.offset_ms = (u32)clampi((int)st.p.offset_ms + ss, 0, 20);
            else if (i == 1) st.p.gap_ms = (u32)clampi((int)st.p.gap_ms + ss * 10, 20, 500);
            st.dirty = 1;
        }
    }
}

static void say_note(u32 n, u32 ms) { st.note = n; st.note_at = ms | 1u; }

static void read_pairs(u32 table, u32* out) {
    for (int k = 0; k < KEYS; ++k) { out[2 * k] = rd32(table + k * KEY_BYTES + 4); out[2 * k + 1] = rd32(table + k * KEY_BYTES + 8); }
}

// The 48 words written in the planned order. Answers the words written.
static int write_pairs(u32 table, const u32* old, const u32* now) {
    const int n = plan(old, now, writes);
    for (int i = 0; i < n; ++i) wr32(table + writes[i].key * KEY_BYTES + (writes[i].which ? 8 : 4), writes[i].value);
    return n;
}

// The sample still the one analysed, right before a write.
static int same_sample(Where& w) {
    return locate(w) == OK && w.slot == st.at.slot && w.obj == st.at.obj && w.data == st.at.data && w.words == st.at.words;
}

static void apply(u32 ms) {
    Where w;
    if (st.state != READY) { say_note(N_NOT_READY, ms); return; }
    if (!same_sample(w)) { say_note(N_CHANGED, ms); st.state = WAITING; return; }
    const u32 s = w.slot;
    read_pairs(w.table, current);
    // the kit's own pairs are kept unless this slot's sample is the one kept for and the table still holds our words
    int ours = st.orig_valid[s] && st.orig_words[s] == w.words && st.written_valid[s];
    for (int i = 0; ours && i < 2 * KEYS; ++i) if (current[i] != st.written[s][i]) ours = 0;
    if (!ours) {
        for (int i = 0; i < 2 * KEYS; ++i) st.orig[s][i] = current[i];
        st.orig_words[s] = w.words; st.orig_valid[s] = 1;
    }
    targets(sl, key_slice, st.orig[s], target, capacity_of(w.stereo));   // v6: the mono capacity for a mono sample
    const int n = write_pairs(w.table, current, target);
    for (int i = 0; i < 2 * KEYS; ++i) st.written[s][i] = target[i];
    st.written_valid[s] = 1;
    ++st.applied;
    say_note(N_APPLIED, ms);
    op1f_log(3, LOG_FILE, 3, "veffchop: applied to slot %u: %s, %u slices, %s, %d words written%s", s + 1,
             MODE_TEXT[st.p.mode], sl.n, MAP_TEXT[st.p.map], n, ours ? "" : " (the kit's own pairs kept for undo)");
    for (int q = 0; q < 4; ++q) {
        const u32* t = target + q * 12;
        op1f_log(3, LOG_FILE, 4, "veffchop: keys %u-%u %08x-%08x %08x-%08x %08x-%08x %08x-%08x %08x-%08x %08x-%08x",
                 q * 6 + 1, q * 6 + 6, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11]);
    }
}

static void undo(u32 ms) {
    Where w;
    if (locate(w) != OK || !st.orig_valid[w.slot] || st.orig_words[w.slot] != w.words) { say_note(N_NOTHING_TO_UNDO, ms); return; }
    const u32 s = w.slot;
    read_pairs(w.table, current);
    const int n = write_pairs(w.table, current, st.orig[s]);
    st.written_valid[s] = 0;
    st.orig_valid[s] = 0;
    say_note(N_UNDONE, ms);
    op1f_log(3, LOG_FILE, 5, "veffchop: undo on slot %u: the kit's own pairs back, %d words written", s + 1, n);
}

// ---------------------------------------------------------------- v7: LAZY, the taps
// ARM: the kit's own pairs kept (as for undo), key 1 made the whole sample, so that the lowest key plays it through.
static void lazy_arm(u32 ms) {
    Where w;
    if (st.state != READY) { say_note(N_NOT_READY, ms); return; }
    if (!same_sample(w)) { say_note(N_CHANGED, ms); st.state = WAITING; return; }
    const u32 s = w.slot;
    read_pairs(w.table, current);
    int ours = st.orig_valid[s] && st.orig_words[s] == w.words && st.written_valid[s];
    for (int i = 0; ours && i < 2 * KEYS; ++i) if (current[i] != st.written[s][i]) ours = 0;
    if (!ours) {
        for (int i = 0; i < 2 * KEYS; ++i) st.orig[s][i] = current[i];
        st.orig_words[s] = w.words; st.orig_valid[s] = 1;
    }
    for (int i = 0; i < 2 * KEYS; ++i) target[i] = current[i];
    target[0] = 0;
    target[1] = q31_of_frame(w.frames - 1u, capacity_of(w.stereo));
    const int n = write_pairs(w.table, current, target);
    for (int i = 0; i < 2 * KEYS; ++i) st.written[s][i] = target[i];
    st.written_valid[s] = 1;
    st.armed = 1; st.armed_slot = s; st.tapped_seen = tapped; st.playing = 0; st.play_frame = 0;
    lazy::clear(st.taps); st.dirty = 1;
    lazy_armed = 1;
    op1f_log(3, LOG_FILE, 16, "veffchop: lazy armed on slot %u: key 1 plays the whole sample, frames 0-%u (%08x), %d words written; play key 1 and tap",
             s + 1, w.frames - 1u, target[1], n);
}

// DISARM: the kit's own pairs back (`restore` 1), or only the state let go (after APPLY, or a changed sample).
static void lazy_disarm(u32 ms, int restore, const char* why) {
    if (!st.armed) return;
    st.armed = 0; lazy_armed = 0; lazy_playing = 0;
    if (restore) undo(ms);
    op1f_log(3, LOG_FILE, 17, "veffchop: lazy disarmed on slot %u: %s, %u tap(s)", st.armed_slot + 1, why, st.taps.n);
}

// While armed, a frame: where the voice on key 1 is, and the taps made since.
static void lazy_frame() {
    if (!st.armed) { lazy_playing = 0; return; }
    u32 keys[lazy::VOICES], states[lazy::VOICES], positions[lazy::VOICES];
    for (u32 v = 0; v < (u32)lazy::VOICES; ++v) {
        const u32 at = VOICE0 + v * (u32)lazy::VOICE_BYTES;
        keys[v] = rd32(VOICE_KEYS + 4u * v);
        states[v] = rd32(at + (u32)lazy::STATE_AT);
        positions[v] = rd32(at + (u32)lazy::POS_AT);
    }
    const int v = lazy::playing(keys, states, positions, 0u);
    const u32 was = st.playing;
    st.playing = v >= 0;
    if (v >= 0) st.play_frame = lazy::frame_of_position(positions[v]);
    if (st.playing != was)
        op1f_log(3, LOG_FILE, 18, "veffchop: lazy: key 1 %s (voice %d, frame %u), %u tap(s)", st.playing ? "plays" : "ended", v, st.play_frame, st.taps.n);
    lazy_playing = st.playing;
    const u32 t = tapped;
    if (t == st.tapped_seen) return;
    const u32 count = t - st.tapped_seen;
    st.tapped_seen = t;
    for (u32 k = 0; k < count && st.playing; ++k) {
        const u32 f = snap(an, st.play_frame < an.frames ? st.play_frame : an.frames - 1u);
        const int took = lazy::add(st.taps, f, an.frames, frames_of_ms((u32)lazy::GAP_MS));
        if (st.taps_logged < 64u) {
            ++st.taps_logged;
            op1f_log(3, LOG_FILE, 19, "veffchop: lazy tap at frame %u (heard %u): %s, %u cut(s)", f, st.play_frame, took ? "a cut" : "too close, left out", st.taps.n);
        }
        if (took) st.dirty = 1;
    }
}

// ---------------------------------------------------------------- v8: EDIT one key; COPY and PASTE its sound
enum { CLIP_ASK = 1, SNAP_REACH = 512, READS_PER_COL = 32, PASTE_BUDGET = 4096 };
enum { EN_NONE = 0, EN_STRIKE = 1, EN_COPIED = 2, EN_NO_MEMORY = 3, EN_COPY_LONG = 4, EN_NOTHING_COPIED = 5, EN_MISSING = 6,
       EN_PASTED = 7, EN_KEPT = 8, EN_NOT_KEPT = 9, EN_BUSY = 10 };
static const char* const ED_NOTE_TEXT[] = { "", "STRIKE A KEY FIRST", "COPIED", "NO MEMORY FOR A COPY", "TOO LONG TO COPY", "NOTHING COPIED",
                                            "", "PASTED", "PASTED AND KEPT", "PASTED, NOT KEPT", "BUSY" };
static void ed_say(u32 n, u32 ms) { st.ed_note = n; st.ed_note_at = ms | 1u; }

static u32 read_l16(u32 data, u32 f, u32 stereo, u32 ch) {   // one channel of frame f, as an unsigned halfword
    const u32 a = data + (u32)DATA_PAD + (stereo ? f * 4u + ch * 2u : f * 2u);
    const u32 w = rd32(a & ~3u);
    return (a & 2u) ? w >> 16 : w & 0xffffu;
}

// The kit's own pairs kept for undo, as at an apply, and the table's words taken as ours from here.
static void ensure_orig(const Where& w) {
    const u32 s = w.slot;
    read_pairs(w.table, current);
    int ours = st.orig_valid[s] && st.orig_words[s] == w.words && st.written_valid[s];
    for (int i = 0; ours && i < 2 * KEYS; ++i) if (current[i] != st.written[s][i]) ours = 0;
    if (!ours) {
        for (int i = 0; i < 2 * KEYS; ++i) st.orig[s][i] = current[i];
        st.orig_words[s] = w.words; st.orig_valid[s] = 1;
    }
    for (int i = 0; i < 2 * KEYS; ++i) st.written[s][i] = current[i];
    st.written_valid[s] = 1;
}

// The end of the slot's material (edit_paste.h's `used`): its own length, the furthest key's end, our own paste.
static u32 material_end(const Where& w) {
    u32 ends[KEYS];
    const u32 cap = capacity_of(w.stereo);
    for (int k = 0; k < KEYS; ++k) ends[k] = frame_of_q31(rd32(w.table + k * KEY_BYTES + 8), cap);
    const u32 pasted = st.paste_obj[w.slot] == w.obj && st.paste_words[w.slot] == w.words ? st.paste_end[w.slot] : 0u;
    // v10: after a drop (the base's `dropped`, while no sound was loaded since) the material is the cut: the firmware's
    // words still say the sample loaded before, and the keys' ends still point into it (stale audio a paste may cover)
    if (dropped_now[w.slot & 7u]) return edit::used(dropped_now[w.slot & 7u], ends, 0u, pasted);
    return edit::used(w.frames, ends, KEYS, pasted);
}

// One key's start and end written now: the two words in the order that keeps the pair in order.
static void write_key(const Where& w, u32 key, u32 start, u32 end) {
    const u32 cap = capacity_of(w.stereo);
    read_pairs(w.table, current);
    for (int i = 0; i < 2 * KEYS; ++i) target[i] = current[i];
    target[2 * key] = q31_of_frame(start, cap);
    target[2 * key + 1] = q31_of_frame(end, cap);
    if (target[2 * key + 1] < target[2 * key] + (u32)MIN_Q) target[2 * key + 1] = target[2 * key] + (u32)MIN_Q;
    write_pairs(w.table, current, target);
    for (int i = 0; i < 2 * KEYS; ++i) st.written[w.slot][i] = target[i];
    st.written_valid[w.slot] = 1;
}

static void edit_load(const Where& w, u32 key) {
    const u32 cap = capacity_of(w.stereo);
    st.ed_slot = w.slot; st.ed_key = key; st.ed_stereo = w.stereo; st.ed_data = w.data;
    st.ed_start = frame_of_q31(rd32(w.table + key * KEY_BYTES + 4), cap);
    st.ed_end = frame_of_q31(rd32(w.table + key * KEY_BYTES + 8), cap);
    st.ed_total = material_end(w);
    if (st.ed_end + 1u > st.ed_total) st.ed_total = st.ed_end + 1u;                // v10: a key past the material is still edited whole
    if (st.ed_total > cap) st.ed_total = cap;
    st.ed_centre = st.ed_start + ((st.ed_end - st.ed_start) >> 1);
    st.zc_span = 0;                                                                 // the zoomed wave read anew
}

static void edit_enter(u32 ms) {
    Where w;
    if (st.state != READY || !same_sample(w)) { say_note(N_NOT_READY, ms); return; }
    ensure_orig(w);
    st.editing = 1; st.ed_zoom = 0;
    edit_load(w, st.struck_valid ? st.struck_key : 0u);                            // v10: key 1 when none was struck
    op1f_log(3, LOG_FILE, 21, "veffchop: edit on slot %u, key %u: frames %u-%u of %u", w.slot + 1, st.ed_key + 1, st.ed_start, st.ed_end, st.ed_total);
}

static void edit_leave(const char* why) {
    if (!st.editing) return;
    st.editing = 0;
    op1f_log(3, LOG_FILE, 22, "veffchop: edit left: %s; key %u frames %u-%u", why, st.ed_key + 1, st.ed_start, st.ed_end);
}

// The nearest zero crossing of the sum around frame f, within SNAP_REACH frames; f when none.
static u32 snap_zero(u32 f) {
    const u32 lo = f > (u32)SNAP_REACH ? f - (u32)SNAP_REACH : 1u, hi = f + (u32)SNAP_REACH < st.ed_total ? f + (u32)SNAP_REACH : st.ed_total - 1u;
    u32 best = f, dist = 0xffffffffu;
    int prev = 0;
    for (u32 g = lo - 1u; g <= hi; ++g) {
        const int l = (short)read_l16(st.ed_data, g, st.ed_stereo, 0), r = st.ed_stereo ? (short)read_l16(st.ed_data, g, 1, 1) : l;
        const int sgn = l + r >= 0 ? 1 : -1;
        if (g >= lo && sgn != prev) { const u32 d = g > f ? g - f : f - g; if (d < dist) { dist = d; best = g; } }
        prev = sgn;
    }
    return best;
}

static void edit_write(u32 ms) {
    Where w;
    if (!same_sample(w) || w.slot != st.ed_slot) { edit_leave("the sample changed"); say_note(N_CHANGED, ms); return; }
    edit::in_order(st.ed_start, st.ed_end, st.ed_total ? st.ed_total - 1u : 0u, (int)st.ed_moved);
    write_key(w, st.ed_key, st.ed_start, st.ed_end);
}

static void edit_knobs() {
    const u32 sp = edit::span(st.ed_total, st.ed_zoom), stp = edit::step(sp, (u32)COLS), most = st.ed_total ? st.ed_total - 1u : 0u;
    int changed = 0;
    for (int i = 0; i < 4; ++i) {
        // v10: a turn with shift (or with shift taken as held) moves the same: EDIT has no shift-turns
        const int s = steps_of(turned[i], st.turn_seen[i], st.turn_rest[i]) + steps_of(turned_shift[i], st.turn_shift_seen[i], st.turn_shift_rest[i]);
        if (!s) continue;
        if (st.ed_logged < 64u) {
            ++st.ed_logged;
            op1f_log(3, LOG_FILE, 32, "veffchop: edit knob %d by %d (a step %u frames): start %u end %u zoom %u", i + 1, s, stp, st.ed_start, st.ed_end, st.ed_zoom);
        }
        if (i == 0) { st.ed_start = edit::moved(st.ed_start, s, stp, most); st.ed_moved = 0; st.ed_centre = st.ed_start; changed = 1; }
        else if (i == 1) { st.ed_end = edit::moved(st.ed_end, s, stp, most); st.ed_moved = 1; st.ed_centre = st.ed_end; changed = 1; }
        else if (i == 2) st.ed_zoom = (u32)clampi((int)st.ed_zoom + s, 0, edit::ZOOMS - 1);
        else {
            const u32 len = st.ed_end - st.ed_start;
            const u32 ns = edit::moved(st.ed_start, s, stp, most > len ? most - len : 0u);
            st.ed_start = ns; st.ed_end = ns + len; st.ed_moved = 0;
            st.ed_centre = ns; changed = 1;
        }
    }
    if (changed) edit_write(0);
}

static void edit_snap() {
    st.ed_start = snap_zero(st.ed_start);
    st.ed_end = snap_zero(st.ed_end);
    st.ed_moved = 1;
    edit_write(0);
    op1f_log(3, LOG_FILE, 23, "veffchop: key %u snapped to zero crossings: frames %u-%u", st.ed_key + 1, st.ed_start, st.ed_end);
}

// The zoomed wave, a few columns a frame within the budget of words.
static void zoom_step() {
    const u32 sp = edit::span(st.ed_total, st.ed_zoom);
    const u32 first = edit::view_first(st.ed_total, sp, st.ed_centre);
    if (sp != st.zc_span || first != st.zc_first || st.zc_slot != st.ed_slot) {
        st.zc_span = sp; st.zc_first = first; st.zc_slot = st.ed_slot; st.zc_next = 0;
        for (int i = 0; i < COLS; ++i) st.zc[i] = 0;
    }
    u32 budget = (u32)BUDGET;
    const u32 per = udiv(sp << 4, (u32)COLS);                                       // frames a column, 4 bits behind the point
    while (st.zc_next < (u32)COLS && budget >= (u32)READS_PER_COL) {
        const u32 c = st.zc_next++;
        const u32 a = first + ((c * per) >> 4), z = first + (((c + 1u) * per) >> 4);
        u32 stride = 1;
        while (stride * (u32)READS_PER_COL < z - a) stride <<= 1;
        u32 pk = 0;
        for (u32 f = a; f < z && f < st.ed_total; f += stride) {
            const int v = (short)read_l16(st.ed_data, f, st.ed_stereo, 0);
            const u32 m = (u32)(v < 0 ? -v : v);
            if (m > pk) pk = m;
            --budget;
        }
        st.zc[c] = (u16)pk;
    }
}

// COPY: the selected key's frames into the pack's memory, a budget a frame.
static void copy_begin(u32 ms) {
    short* clip = (short*)op1f_memory(&op1f_card_veffchop, (unsigned)CLIP_ASK);
    if (!clip) { ed_say(EN_NO_MEMORY, ms); return; }
    const u32 n = st.ed_end - st.ed_start + 1u;
    if (n > edit::clip_room(st.ed_stereo)) {
        ed_say(EN_COPY_LONG, ms);
        op1f_log(2, LOG_FILE, 24, "veffchop: copy refused: key %u holds %u frames, the copy %u at most", st.ed_key + 1, n, edit::clip_room(st.ed_stereo));
        return;
    }
    st.copy_state = 1; st.copy_at = 0; st.copy_from = st.ed_start; st.copy_n = n; st.clip_frames = 0; st.clip_stereo = st.ed_stereo;
}

static void copy_step(u32 ms) {
    if (!st.copy_state) return;
    short* clip = (short*)op1f_memory(&op1f_card_veffchop, (unsigned)CLIP_ASK);
    const u32 ch = st.clip_stereo ? 2u : 1u;
    u32 k = 0;
    for (; k < (u32)PASTE_BUDGET && st.copy_at < st.copy_n; ++k, ++st.copy_at) {
        const u32 f = st.copy_from + st.copy_at;
        clip[st.copy_at * ch] = (short)read_l16(st.ed_data, f, st.clip_stereo, 0);
        if (ch == 2u) clip[st.copy_at * 2u + 1u] = (short)read_l16(st.ed_data, f, 1, 1);
    }
    if (st.copy_at >= st.copy_n) {
        st.copy_state = 0; st.clip_frames = st.copy_n;
        ed_say(EN_COPIED, ms);
        op1f_log(3, LOG_FILE, 25, "veffchop: copied key %u of slot %u: %u frames, %s", st.ed_key + 1, st.ed_slot + 1, st.clip_frames, st.clip_stereo ? "stereo" : "mono");
    }
}

// PASTE: the copy behind the slot's material, a budget a frame; then the key set to it, last; then the unit asked to
// keep the sample (op1f_sample_changed).
static void paste_begin(u32 ms) {
    Where w;
    if (!st.clip_frames || st.copy_state) { ed_say(EN_NOTHING_COPIED, ms); return; }
    if (st.paste_state) { ed_say(EN_BUSY, ms); return; }
    if (!same_sample(w) || w.slot != st.ed_slot) { say_note(N_CHANGED, ms); return; }
    const u32 used = material_end(w);
    u32 at = 0, missing = 0;
    if (!edit::place(used, st.clip_frames, w.stereo, at, missing)) {
        st.ed_missing = missing; ed_say(EN_MISSING, ms);
        op1f_log(2, LOG_FILE, 26, "veffchop: paste refused on slot %u key %u: %u frames from frame %u, %u frames (%u ms) more than the slot holds",
                 w.slot + 1, st.ed_key + 1, st.clip_frames, used, missing, udiv(missing, 44u));
        return;
    }
    st.paste_state = 1; st.paste_slot = w.slot; st.paste_key = st.ed_key; st.paste_at = at; st.paste_n = st.clip_frames;
    st.paste_done = 0; st.paste_stereo = w.stereo; st.paste_data = w.data; st.paste_ms = ms;
    op1f_log(3, LOG_FILE, 27, "veffchop: paste onto slot %u key %u: %u frames (%s into %s) at frame %u, behind the material's end %u",
             w.slot + 1, st.ed_key + 1, st.paste_n, st.clip_stereo ? "stereo" : "mono", w.stereo ? "stereo" : "mono", at, used);
}

static void paste_step(u32 ms) {
    if (st.paste_state == 1) {
        const short* clip = (const short*)op1f_memory(&op1f_card_veffchop, (unsigned)CLIP_ASK);
        const u32 base = st.paste_data + (u32)DATA_PAD;
        u32 k = 0;
        while (k < (u32)PASTE_BUDGET && st.paste_done < st.paste_n) {
            short in[2], out[2] = {0, 0}, out2[2] = {0, 0};
            const u32 i = st.paste_done;
            in[0] = clip[i * (st.clip_stereo ? 2u : 1u)]; in[1] = st.clip_stereo ? clip[i * 2u + 1u] : in[0];
            edit::convert(in, st.clip_stereo, st.paste_stereo, out);
            if (st.paste_stereo) {
                pool_wr32(base + (st.paste_at + i) * 4u, (u32)(unsigned short)out[0] | ((u32)(unsigned short)out[1] << 16));
                ++st.paste_done;
            } else {                                                                // two mono frames a word (at is even)
                out2[0] = 0;
                if (i + 1u < st.paste_n) {
                    in[0] = clip[(i + 1u) * (st.clip_stereo ? 2u : 1u)]; in[1] = st.clip_stereo ? clip[(i + 1u) * 2u + 1u] : in[0];
                    edit::convert(in, st.clip_stereo, 0, out2);
                }
                pool_wr32(base + (st.paste_at + i) * 2u, (u32)(unsigned short)out[0] | ((u32)(unsigned short)out2[0] << 16));
                st.paste_done += 2u;
            }
            k += 1u;
        }
        if (st.paste_done < st.paste_n) return;
        Where w;
        if (!same_sample(w) || w.slot != st.paste_slot) {                          // the kit changed under us: no key is set
            st.paste_state = 0; say_note(N_CHANGED, ms);
            op1f_log(2, LOG_FILE, 28, "veffchop: paste on slot %u not finished: the sample changed", st.paste_slot + 1);
            return;
        }
        ensure_orig(w);
        u32 s0 = st.paste_at, e0 = st.paste_at + st.paste_n - 1u;
        write_key(w, st.paste_key, s0, e0);                                         // the key last: nothing played the frames before
        st.paste_end[w.slot] = st.paste_at + st.paste_n; st.paste_obj[w.slot] = w.obj; st.paste_words[w.slot] = w.words;
        if (st.editing && st.ed_key == st.paste_key && st.ed_slot == w.slot) { edit_load(w, st.paste_key); }
        const int t = op1f_sample_changed(&op1f_card_veffchop, (unsigned)ASK, (u32)OP1F_PLACE_DRUM + w.slot);
        op1f_log(3, LOG_FILE, 29, "veffchop: pasted onto slot %u key %u: frames %u-%u, after %u ms; the unit asked to keep it: %d",
                 w.slot + 1, st.paste_key + 1, s0, e0, ms - st.paste_ms, t);
        if (t > 0) { st.paste_ticket = (u32)t; st.paste_state = 2; ed_say(EN_PASTED, ms); }
        else { st.paste_state = 0; ed_say(EN_NOT_KEPT, ms); }
        return;
    }
    if (st.paste_state == 2) {
        Op1fSample got;
        if (op1f_sample_get(&op1f_card_veffchop, (u32)OP1F_PLACE_DRUM + st.paste_slot, &got) != 0 || got.done != st.paste_ticket) {
            if (ms - st.paste_ms > 15000u) { st.paste_state = 0; ed_say(EN_NOT_KEPT, ms); op1f_log(2, LOG_FILE, 30, "veffchop: keep %u: no answer", st.paste_ticket); }
            return;
        }
        st.paste_state = 0;
        ed_say(got.answer >= 0 ? EN_KEPT : EN_NOT_KEPT, ms);
        op1f_log(3, LOG_FILE, 30, "veffchop: keep %u of slot %u: answer %d", st.paste_ticket, st.paste_slot + 1, got.answer);
    }
}

static void edit_frame(u32 ms) {
    copy_step(ms);
    paste_step(ms);
    if (!st.editing) return;
    zoom_step();
    u32 keys[lazy::VOICES], states[lazy::VOICES], positions[lazy::VOICES];
    for (u32 v = 0; v < (u32)lazy::VOICES; ++v) {
        const u32 at = VOICE0 + v * (u32)lazy::VOICE_BYTES;
        keys[v] = rd32(VOICE_KEYS + 4u * v); states[v] = rd32(at + (u32)lazy::STATE_AT); positions[v] = rd32(at + (u32)lazy::POS_AT);
    }
    const int v = lazy::playing(keys, states, positions, st.ed_key);
    st.ed_playing = v >= 0;
    if (v >= 0) st.ed_play_frame = lazy::frame_of_position(positions[v]);
}

static void refuse(u32 why) {
    st.state = REFUSED;
    if (st.why != why || !st.why_logged) {
        st.why = why; st.why_logged = 1;
        op1f_log(2, LOG_FILE, 2, "veffchop: not chopping: %s (slot %u words %u)", WHY_TEXT[why], st.at.slot + 1, st.at.words);
    }
}

// The sample watched: steady for STEADY_MS over two checks or more, then read a piece a frame.
static void watch(u32 ms) {
    Where w;
    const int why = locate(w);
    const int same = w.slot == st.at.slot && w.obj == st.at.obj && w.data == st.at.data && w.words == st.at.words && w.stereo == st.at.stereo;
    if (why != OK && why != NO_SLOT && why != NO_OBJECT) {
        // v5: a kit being loaded reads as no sample, or as mono, for a moment (the unit showed a stereo kit's words with
        // its stereo byte still 0): judged only once its words and its stereo byte stayed put for STEADY_MS over two
        // checks, and a sample too long for a mono one is never called mono.
        if (!same) {
            st.at.slot = w.slot; st.at.obj = w.obj; st.at.data = w.data; st.at.words = w.words; st.at.stereo = w.stereo;
            st.state = WAITING; st.seen_since = ms; st.seen_checks = 0;
            return;
        }
        if (st.state == REFUSED) return;                                             // judged already, nothing moved
        if (st.state != WAITING) { st.state = WAITING; st.seen_since = ms; st.seen_checks = 0; return; }
        ++st.seen_checks;
        if (ms - st.seen_since < (u32)STEADY_MS || st.seen_checks < 2) return;
        refuse((u32)why);
        return;
    }
    if (why != OK) {
        st.at.slot = w.slot; st.at.obj = w.obj; st.at.data = w.data; st.at.words = w.words; st.at.stereo = w.stereo;
        refuse((u32)why);
        return;
    }
    if (!same) {
        st.at.slot = w.slot; st.at.obj = w.obj; st.at.data = w.data; st.at.words = w.words;
        st.at.stereo = w.stereo; st.at.frames = w.frames; st.at.table = w.table;
        st.state = STEADYING; st.seen_since = ms; st.seen_checks = 0; st.why_logged = 0;
        return;
    }
    if (st.state == REFUSED || st.state == WAITING) { st.state = STEADYING; st.seen_since = ms; st.seen_checks = 0; }
    if (st.state == STEADYING) {
        ++st.seen_checks;
        if (ms - st.seen_since < (u32)STEADY_MS || st.seen_checks < 2) return;
        begin(an, st.at.frames, st.at.stereo);
        st.state = ANALYSING; st.analyse_frames = 0;
        op1f_log(3, LOG_FILE, 6, "veffchop: slot %u: %u frames, %s%s; analysing %u windows, %u frames a frame",
                 st.at.slot + 1, st.at.frames, st.at.stereo ? "stereo" : "mono", dropped_now[st.at.slot & 7u] ? ", a drop" : "", an.windows, (u32)BUDGET);
        return;
    }
    if (st.state == ANALYSING) {
        ++st.analyse_frames;
        if (step(an, sample_rd, st.at.data + DATA_PAD, BUDGET)) st.state = FINISHING;
        return;
    }
    if (st.state == FINISHING) {                         // v2: the candidates built over frames
        ++st.analyse_frames;
        if (finish_step(an, FINISH_BUDGET)) {
            st.state = READY; st.dirty = 1;
            op1f_log(3, LOG_FILE, 7, "veffchop: slot %u analysed in %u frames: %u candidates, loudest window %u",
                     st.at.slot + 1, st.analyse_frames, an.ncand, an.peak_env);
        }
    }
}

// ---------------------------------------------------------------- painting
enum { WX = 12, WW = 456, WY0 = 44, WMID = 86, WHALF = 36, STRIP_Y = 128, STRIP_H = 16, CELL = 19 };

static int put(char* out, int at, const char* t) { while (*t && at < 30) out[at++] = *t++; out[at] = 0; return at; }
static int put_num(char* out, int at, u32 n) {
    char d[12];
    int k = 0;
    do { const u32 tens = udiv(n, 10u); d[k++] = (char)('0' + n - tens * 10u); n = tens; } while (n && k < 11);
    while (k && at < 30) out[at++] = d[--k];
    out[at] = 0;
    return at;
}

static unsigned slice_colour(int s, int bright) {
    if (s < 0) return VEFF_DARK;
    if (bright) return VEFF_PINK;
    return (s & 1) ? veffui::white(150) : veffui::pink(150);
}

// x of a frame on the wave
static int x_of(u32 f) {
    const u32 scale = an.frames ? udiv((u32)WW << 20, an.frames) : 0;   // once a frame: a bit loop
    return WX + (int)((f * scale) >> 20);
}

// v8: a length in frames as milliseconds, with a tenth (44.1 frames a ms)
static int put_ms(char* out, int at, u32 frames) {
    const u32 tenths = udiv(frames * 10u + 22u, 44u);
    const u32 ms = udiv(tenths, 10u);
    at = put_num(out, at, ms);
    at = put(out, at, ".");
    at = put_num(out, at, tenths - ms * 10u);
    return put(out, at, " MS");
}

// v8: the EDIT view: the zoomed wave, the key's start (white) and end (pink), where it plays.
static void paint_edit(void* c, u32 ms, int shift) {
    char tag[32], v0[32], v1[32], v2[32], v3[32];
    tag[0] = 0;
    if (st.ed_note && ms - st.ed_note_at < (u32)NOTE_MS) {
        if (st.ed_note == EN_MISSING) put(tag, put_ms(tag, put(tag, 0, "NEEDS "), st.ed_missing), " MORE");
        else put(tag, 0, ED_NOTE_TEXT[st.ed_note]);
    } else if (st.copy_state) put(tag, 0, "COPYING");
    else if (st.paste_state == 1) put(tag, 0, "PASTING");
    else if (st.paste_state == 2) put(tag, 0, "KEEPING");
    else put_num(tag, put(tag, 0, "EDIT KEY "), st.ed_key + 1u);
    veffui::Column col[4];
    const u32 sp = edit::span(st.ed_total, st.ed_zoom);
    if (shift) {
        put_num(v0, put(v0, 0, "COPY|KEY "), st.ed_key + 1u);                   // v10: the shift-presses, as hints
        if (st.clip_frames) put(v1, put_ms(v1, put(v1, 0, "PASTE|"), st.clip_frames), ""); else put(v1, 0, "PASTE|NO COPY YET");
        put(v2, 0, "LEAVE|EDIT"); put(v3, 0, "UNDO");
        col[0].label = "SHIFT+PRESS"; col[0].value = v0; col[0].frac = -1;
        col[1].label = "SHIFT+PRESS"; col[1].value = v1; col[1].frac = -1;
        col[2].label = "SHIFT+PRESS"; col[2].value = v2; col[2].frac = -1;
        col[3].label = "SHIFT+PRESS"; col[3].value = v3; col[3].frac = -1;
    } else {
        put_ms(v0, 0, st.ed_start); put_ms(v1, 0, st.ed_end - st.ed_start + 1u); put_ms(v2, 0, sp); put(v3, 0, "SNAP");
        col[0].label = "START"; col[0].value = v0; col[0].frac = st.ed_total ? (int)udiv((st.ed_start >> 6) * 1024u, (st.ed_total >> 6) + 1u) : 0;
        col[1].label = "LENGTH"; col[1].value = v1; col[1].frac = st.ed_total ? (int)udiv((st.ed_end >> 6) * 1024u, (st.ed_total >> 6) + 1u) : 0;
        col[2].label = "ZOOM"; col[2].value = v2; col[2].frac = (int)udiv(st.ed_zoom * 1024u, (u32)edit::ZOOMS - 1u);
        col[3].label = "NUDGE"; col[3].value = v3; col[3].frac = 512;
    }
    veffui::page(c, "CHOP", -1, col, tag, 1);
    const u32 first = st.zc_first, span = st.zc_span ? st.zc_span : 1u;
    const u32 scale = udiv((u32)WW << 16, span);                                    // pixels a frame, 16 bits behind the point
    op1f_paint_line(c, WX, WMID, WX + WW - 1, WMID, 1, VEFF_DARK);
    u32 peak = 64;                                                                  // the view drawn to its own loudest column
    for (int i = 0; i < COLS; ++i) if (st.zc[i] > peak) peak = st.zc[i];
    const u32 hscale = udiv((u32)WHALF << 16, peak);
    for (int i = 0; i < COLS; ++i) {
        const u32 h = (st.zc[i] * hscale) >> 16;
        if (!h) continue;
        const u32 f = first + udiv((u32)i * span, (u32)COLS);
        const int in_key = f >= st.ed_start && f <= st.ed_end;
        op1f_paint_box(c, WX + 2 * i, WMID - (int)h, 2, 2 * (int)h + 1, in_key ? VEFF_PINK : VEFF_GREY);
    }
    if (st.ed_start >= first && st.ed_start < first + span) {
        const int x = WX + (int)(((st.ed_start - first) * scale) >> 16);
        op1f_paint_line(c, x, WY0 - 4, x, WMID + WHALF + 4, 1, VEFF_WHITE);
    }
    if (st.ed_end >= first && st.ed_end < first + span) {
        const int x = WX + (int)(((st.ed_end - first) * scale) >> 16);
        op1f_paint_line(c, x, WY0 - 4, x, WMID + WHALF + 4, 1, VEFF_PINK);
    }
    if (st.ed_playing && st.ed_play_frame >= first && st.ed_play_frame < first + span) {
        const int x = WX + (int)(((st.ed_play_frame - first) * scale) >> 16);
        op1f_paint_box(c, x, WY0 - 2, 2, WHALF * 2 + 6, VEFF_WHITE);
    }
    // where the view lies in the whole: a bar under the wave
    const u32 whole = st.ed_total ? st.ed_total : 1u;
    const int bx = WX + (int)udiv((first >> 6) * (u32)WW, (whole >> 6) + 1u), bw = 1 + (int)udiv((span >> 6) * (u32)WW, (whole >> 6) + 1u);
    op1f_paint_box(c, WX, STRIP_Y + 6, WW, 2, VEFF_DARK);
    op1f_paint_box(c, bx, STRIP_Y + 4, bw, 6, VEFF_WHITE);
}

static void paint(void* c, u32 ms, int shift) {
    op1f_paint_box(c, 0, 0, 480, 224, OP1F_COLOUR(0, 0, 0));
    if (st.editing && page_up()) { paint_edit(c, ms, shift); return; }   // v8
    char tag[32];
    tag[0] = 0;
    const int ready = st.state == READY;
    if (!page_up()) put(tag, 0, "DRUM ONLY");
    else if (st.armed) put(tag, 0, st.playing ? "TAP!" : "ARMED: PLAY KEY 1");          // v7
    else if (drop_ticket) put(tag, 0, "DROPPING");                                    // v6
    else if (drop_message && ms - drop_message_at < (u32)NOTE_MS && drop_message == DM_DROPPED) put(tag, 0, "DROPPED");
    else if (drop_message && ms - drop_message_at < (u32)NOTE_MS && drop_reason == OP1F_EMPTY) put(tag, 0, "NOTHING LIFTED");
    else if (drop_message && ms - drop_message_at < (u32)NOTE_MS) put_num(tag, put(tag, 0, "DROP REFUSED -"), (u32)(-drop_reason));
    else if (st.note && ms - st.note_at < (u32)NOTE_MS) put(tag, 0, NOTE_TEXT[st.note]);
    else if (st.state == FINISHING) put(tag, 0, "ANALYSING 100%");
    else if (st.state == ANALYSING) put(tag, put_num(tag, put(tag, 0, "ANALYSING "), progress(an)), "%");
    else if (st.state == STEADYING) put(tag, 0, "WAITING");
    else if (st.state == REFUSED) put(tag, 0, WHY_TEXT[st.why]);
    else if (ready && st.p.mode == LAZY && st.taps.n == 0) put(tag, 0, "PRESS KNOB 1: ARM");
    else if (ready) put(tag, put_num(tag, 0, sl.n), sl.n == 1 ? " SLICE" : " SLICES");
    if (shift && tag[0] == 0) put(tag, 0, "SHIFT");

    // the columns
    char v0[32], v1[32], v2[32], v3[32];
    veffui::Column col[4];
    if (shift) {
        put(v0, put_num(v0, 0, st.p.offset_ms), " MS");
        put(v1, put_num(v1, 0, st.p.gap_ms), " MS");
        // v10: the shift-presses said where the slider was: knob 3 EDIT, knob 4 UNDO
        if (st.struck_valid) put_num(v2, put(v2, 0, "EDIT|KEY "), st.struck_key + 1u); else put(v2, 0, "EDIT THIS|SAMPLE");
        put(v3, 0, st.orig_valid[st.at.slot & 7] ? "UNDO" : "NOTHING|TO UNDO");
        col[0].label = "OFFSET"; col[0].value = v0; col[0].frac = (int)udiv(st.p.offset_ms * 1024u, 20u);
        col[1].label = "GAP"; col[1].value = v1; col[1].frac = (int)udiv((st.p.gap_ms - 20u) * 1024u, 480u);
        col[2].label = "SHIFT+PRESS"; col[2].value = v2; col[2].frac = -1;
        col[3].label = "SHIFT+PRESS"; col[3].value = v3; col[3].frac = -1;
    } else {
        put(v0, 0, MODE_TEXT[st.p.mode]);
        if (st.p.mode == EQUAL) put_num(v1, 0, st.p.parts);
        else if (st.p.mode == LAZY) put_num(v1, 0, st.taps.n);                      // v7: the taps
        else put_num(v1, 0, st.p.sens);
        put(v2, 0, MAP_TEXT[st.p.map]);
        put(v3, 0, st.applied && st.written_valid[st.at.slot & 7] ? "AGAIN" : "PRESS");
        col[0].label = "MODE"; col[0].value = v0; col[0].frac = (int)st.p.mode * 512;
        col[1].label = st.p.mode == EQUAL ? "PARTS" : st.p.mode == LAZY ? "TAPS" : "SENS"; col[1].value = v1;
        col[1].frac = st.p.mode == EQUAL ? (int)udiv((st.p.parts - 2u) * 1024u, 22u) : st.p.mode == LAZY ? (int)udiv(st.taps.n * 1024u, 23u) :
                      (int)udiv(st.p.sens * 1024u, 100u);
        col[2].label = "MAP"; col[2].value = v2; col[2].frac = (int)udiv(st.p.map * 1024u, 3u);
        col[3].label = "APPLY"; col[3].value = v3; col[3].frac = ready ? 1024 : 0;
    }
    veffui::page(c, "CHOP", -1, col, tag[0] ? tag : 0, 1);
    // the icon: a cut wave
    for (int i = 0; i < 9; ++i) {
        const int h = (i & 1) ? 4 : 9;
        op1f_paint_line(c, 428 + i * 4, 22 - h, 428 + i * 4, 22 + h, 1, VEFF_WHITE);
    }
    op1f_paint_line(c, 444, 8, 444, 36, 1, VEFF_PINK);

    if (!page_up()) {
        veffui::text(c, WX, 70, "T1 OF THE DRUM SAMPLER ONLY", VEFF_WHITE, 2, 2);
        return;
    }
    if (st.state == REFUSED || st.state == WAITING || st.state == STEADYING) {
        veffui::text(c, WX, 70, st.state == REFUSED ? WHY_TEXT[st.why] : "WAITING FOR THE SAMPLE", VEFF_GREY, 3, 3);
        return;
    }

    // which slice each column lies in, and the struck key's slice
    const int struck_slice = ready && st.struck_valid ? key_slice[st.struck_key] : -1;
    op1f_paint_line(c, WX, WMID, WX + WW - 1, WMID, 1, VEFF_DARK);
    u32 peak = 1;
    for (int i = 0; i < COLS; ++i) if (an.col[i] > peak) peak = an.col[i];
    const u32 hscale = udiv((u32)WHALF << 16, peak);
    const u32 fpc = udiv(an.frames, COLS) + 1;                   // frames a column (for its slice)
    int s = 0;
    for (int i = 0; i < COLS; ++i) {
        const u32 h = (an.col[i] * hscale) >> 16;
        if (!h) continue;
        unsigned colour = VEFF_GREY;
        if (ready) {
            const u32 f = (u32)i * fpc;
            while (s + 1 < (int)sl.n && f >= sl.start[s + 1]) ++s;
            colour = slice_colour(s, s == struck_slice);
        }
        op1f_paint_box(c, WX + 2 * i, WMID - (int)h, 2, 2 * (int)h + 1, colour);
    }
    if (st.state == ANALYSING || st.state == FINISHING) {
        const int x = x_of(an.read);
        op1f_paint_line(c, x, WY0, x, WMID + WHALF, 1, VEFF_PINK);
        return;
    }
    // the cuts and their numbers
    int label_end = -100;
    for (u32 k = 0; k < sl.n; ++k) {
        const int x = x_of(sl.start[k]);
        op1f_paint_line(c, x, WY0 - 2, x, WMID + WHALF + 2, 1, VEFF_WHITE);
        char t[4];
        put_num(t, 0, k + 1);
        if (x + 3 >= label_end + 6 || (int)k == struck_slice)          // a number that would touch the one before: left out
            label_end = x + 3 + veffui::text(c, x + 3, WY0 - 9, t, (int)k == struck_slice ? VEFF_PINK : VEFF_WHITE, 2, 1);
    }
    if (st.armed && st.playing) {                                                   // v7: where key 1 plays
        const int x = x_of(st.play_frame);
        op1f_paint_box(c, x, WY0 - 4, 3, WHALF * 2 + 10, VEFF_PINK);
    }
    // the play marker: the struck slice at 44.1 frames a ms (pitch not counted)
    if (struck_slice >= 0) {
        const u32 el = ms - st.struck_at;
        const u32 f = sl.start[struck_slice] + el * 44u + ((el * 26u) >> 8);
        if (el < 30000u && f <= sl.end[struck_slice]) {
            const int x = x_of(f);
            op1f_paint_box(c, x, WY0 - 2, 2, WHALF * 2 + 6, VEFF_PINK);
        }
    }
    // the 24 keys: each its slice's number and colour; the kit's own (unmapped) dark
    for (int k = 0; k < KEYS; ++k) {
        const int x = WX + k * CELL, ks = key_slice[k];
        const int lit = st.struck_valid && (int)st.struck_key == k;
        op1f_paint_box(c, x, STRIP_Y, CELL - 2, STRIP_H, lit ? VEFF_PINK : slice_colour(ks, 0));
        char t[4];
        if (ks >= 0) put_num(t, 0, (u32)ks + 1); else put(t, 0, "-");
        const int tw = veffui::text_width(t, 2, 1);
        veffui::text(c, x + ((CELL - 2 - tw) >> 1), STRIP_Y + 5, t, ks < 0 ? VEFF_GREY : OP1F_COLOUR(8, 8, 8), 2, 1);   // 0 would only measure
    }
}

// ---------------------------------------------------------------- v6: the drop, by the base (docs/sample.md, base 43)
// The input function asks op1f_sample_drop on the drop key's up (it only stores the answer); the frame function reads
// op1f_sample_get until `done` is the ticket, then says how it ended and has the sample analysed again. Nothing of the
// firmware's is called here: the unit does the drum sampler page's own drop in its turn.
static u32 drop_seen;                                    // the last ask said
static u32 drop_place, drop_at;
static u32 drops_done, drops_refused;
enum { DROP_WAIT_MS = 10000 };

static void drop_frame(u32 ms) {
    const u32 asked = drop_asked;
    if (asked != drop_seen) {                            // (count << 8 | place) and the answer
        drop_seen = asked;
        const int answer = drop_answer;
        const u32 place = asked & 0xffu;
        if (answer > 0) {
            drop_ticket = (u32)answer; drop_place = place; drop_at = ms;
            op1f_log(3, LOG_FILE, 13, "veffchop: drop asked into drum %u: ticket %d", place - (u32)OP1F_PLACE_DRUM + 1u, answer);
        } else {
            ++drops_refused;
            drop_message = DM_REFUSED; drop_reason = answer; drop_message_at = ms | 1u;
            op1f_log(2, LOG_FILE, 12, "veffchop: drop into drum %u refused at once: %d (%s)", place - (u32)OP1F_PLACE_DRUM + 1u, answer,
                     answer == OP1F_EMPTY ? "nothing lifted" : answer == OP1F_BUSY ? "busy" : answer == OP1F_NOT_HERE ? "the sound does not ride veffchop" :
                     answer == OP1F_TOO_LARGE ? "too large" : answer == OP1F_BAD_NAME ? "this base drops into synth sounds only" : "see the base's sample line");
        }
    }
    if (!drop_ticket) return;
    Op1fSample got;
    const int r = op1f_sample_get(&op1f_card_veffchop, drop_place, &got);
    if (r == 0 && got.done == drop_ticket) {
        const u32 slot = drop_place - (u32)OP1F_PLACE_DRUM;
        if (got.answer >= 0) { ++drops_done; drop_message = DM_DROPPED; }
        else { ++drops_refused; drop_message = DM_REFUSED; drop_reason = got.answer; }
        drop_message_at = ms | 1u;
        op1f_log(3, LOG_FILE, 14, "veffchop: drop %u into drum %u done: answer %d, dropped %u frames, stereo %u, after %u ms; %u done %u refused",
                 drop_ticket, slot + 1u, got.answer, got.dropped, got.stereo, ms - drop_at, drops_done, drops_refused);
        drop_ticket = 0;
        st.at.obj = 0;                                   // seen anew: analysed again, never applied by itself
        return;
    }
    if (ms - drop_at > (u32)DROP_WAIT_MS) {
        op1f_log(2, LOG_FILE, 15, "veffchop: drop %u: no end after %u ms (get %d, done %u)", drop_ticket, ms - drop_at, r, r == 0 ? got.done : 0u);
        drop_ticket = 0;
    }
}

// ---------------------------------------------------------------- the frame
extern "C" void veffchop_frame(const Op1fFrame* now) OP1F_NAMED("veffchop_frame");
extern "C" void veffchop_frame(const Op1fFrame* now) {
    if (now->bytes < 40u) return;
    words_read = 0;
    if (!st.started) {
        st.started = 1;
        op1f_log(3, LOG_FILE, 1, "veffchop: start");
        defaults();
        return;                                          // nothing else in the first frame
    }
    if (!st.stood && op1f_page_stand_in(&op1f_card_veffchop, ASK, 0)) st.stood = 1;
    if (!analysis) {                                                                // v9: the memory first
        char* m = (char*)op1f_memory(&op1f_card_veffchop, (unsigned)CLIP_ASK);
        if (!m) {
            if (now->page == &op1f_card_veffchop && now->canvas) {
                op1f_paint_box(now->canvas, 0, 0, 480, 224, OP1F_COLOUR(0, 0, 0));
                veffui::text(now->canvas, WX, 70, "WAITING FOR MEMORY", VEFF_GREY, 3, 3);
            }
            return;
        }
        analysis = (Analysis*)(m + edit::CLIP_BYTES);
        op1f_log(3, LOG_FILE, 31, "veffchop: memory at %08x: the copy's room, then the analysis", (u32)(unsigned long)m);
    }
    if (shown_where != st.shown_where_seen || shown_how != st.shown_how_seen) {
        st.shown_where_seen = shown_where; st.shown_how_seen = shown_how;
        op1f_log(3, LOG_FILE, 8, "veffchop: drum engine page %s (where %u, how %u)", page_up() ? "up" : "off", st.shown_where_seen, st.shown_how_seen);
    }
    shift_held = now->shift ? 1u : 0u;                   // v10: the base's word of shift, each frame: its up never comes to us
    const u32 mode_before = st.p.mode;
    knobs();
    if (st.armed && st.p.mode != LAZY && mode_before == LAZY) lazy_disarm(now->ms, 1, "another mode");   // v7
    drop_frame(now->ms);                                 // v6
    const u32 was = st.state;
    watch(now->ms);
    if (st.armed && (st.state != READY || st.at.slot != st.armed_slot)) lazy_disarm(now->ms, 0, "the sample changed");   // v7
    lazy_frame();                                        // v7
    if (st.editing && (st.state != READY || st.at.slot != st.ed_slot || !page_up())) edit_leave("the sample or the page changed");
    edit_frame(now->ms);                                 // v8
    if (st.state == READY && was == READY && st.dirty) {    // v2: never in the frame the analysis ends
        st.dirty = 0;
        if (st.p.mode == LAZY) lazy::slices(st.taps, an.frames, sl);              // v7: LAZY is the taps
        else slice(an, st.p, sl);
        map(an, sl, st.p.map, key_slice);
    }
    for (int i = 0; i < 4; ++i) {                        // presses: the fourth applies, shift and it undoes
        const u32 p = pressed[i], ps = pressed_shift[i];
        const int hit = p != st.press_seen[i], hit_s = ps != st.press_shift_seen[i];
        st.press_seen[i] = p; st.press_shift_seen[i] = ps;
        if (st.editing && page_up()) {                                              // v8: EDIT's own presses
            if (i == 3 && hit) edit_snap();
            if (i == 0 && hit_s) copy_begin(now->ms);
            if (i == 1 && hit_s) paste_begin(now->ms);
            if (i == 2 && hit_s) edit_leave("shift and knob 3");
            if (i == 3 && hit_s) { edit_leave("undo"); undo(now->ms); }
            continue;
        }
        if (i == 2 && hit_s && page_up() && !st.armed) { edit_enter(now->ms); continue; }   // v8: shift and knob 3: EDIT
        if (i == 0 && hit && page_up() && st.p.mode == LAZY) {                     // v7: ARM, and again DISARM
            if (st.armed) lazy_disarm(now->ms, 1, "knob 1 pressed again");
            else lazy_arm(now->ms);
        }
        if (i == 1 && hit && st.armed) tapped = tapped + 1u;                       // v7: knob 2's press taps too
        if (i == 3 && hit && page_up()) {
            apply(now->ms);
            if (st.armed && st.note == N_APPLIED) lazy_disarm(now->ms, 0, "applied");
        }
        if (i == 3 && hit_s && page_up()) {
            if (st.armed) { lazy::clear(st.taps); st.dirty = 1; op1f_log(3, LOG_FILE, 20, "veffchop: lazy taps cleared"); }   // v7
            else undo(now->ms);
        }
    }
    const u32 ph = press4_how;
    if (ph != st.press4_seen) {
        st.press4_seen = ph;
        op1f_log(3, LOG_FILE, 10, "veffchop: 4th knob pressed: shift flag %u, down-shift %u, shift held %u", ph & 1u, (ph >> 1) & 1u, (ph >> 2) & 1u);
    }
    const u32 sk = struck;
    if (sk != st.struck_seen) {
        st.struck_seen = sk;
        const u32 n = sk & 0xffu;
        const int k = drum_key_of(n);
        st.struck_at = now->ms; st.struck_valid = k >= 0; st.struck_key = k >= 0 ? (u32)k : 0;
        if (st.editing && k >= 0 && (u32)k != st.ed_key) {                          // v8: another key struck: it is edited now
            Where w;
            if (same_sample(w)) { edit_load(w, (u32)k); op1f_log(3, LOG_FILE, 21, "veffchop: edit on slot %u, key %u: frames %u-%u of %u", w.slot + 1, st.ed_key + 1, st.ed_start, st.ed_end, st.ed_total); }
        }
        if (n < 64u && !(st.keys_logged[n >> 5] & (1u << (n & 31u)))) {     // each number once: the key map, to settle
            st.keys_logged[n >> 5] |= 1u << (n & 31u);
            op1f_log(3, LOG_FILE, 9, "veffchop: keyboard key %u struck, taken as drum key %d (0: none)", n, k + 1);
        }
    }
    if (now->page != &op1f_card_veffchop || !now->canvas) return;
    paint(now->canvas, now->ms, now->shift != 0);
}

#ifdef VEFFCHOP_HOST
u32 veffchop_words_read_last_frame() { return words_read; }
#endif
