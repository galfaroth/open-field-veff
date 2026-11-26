// veffchop: chops the current drum kit's sample into the 24 drum keys, for OS 1.7.3 (1.7.3+0) only. Version 2: the
// drum's page (base 40, OP1F_ASK_DRUM_PAGE, docs/drum-page.md): its page stands in for the drum sampler's page on T1 in
// the drum's mode; it plays nothing itself and lets every keyboard key through to the kit. veffdrumprobe/FINDINGS.md (sections 6 and 7) says where every address comes from.
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

typedef unsigned u32;
typedef unsigned char u8;
using namespace chop;

static const char LOG_FILE[] = "op1f/veffchop.cpp";
enum { ASK = 0, SLOTS = 8, STEADY_MS = 500, NOTE_MS = 2500,
       OFF_WORDS = 0xd76c, OFF_STEREO = 0xd770, OBJECT_BYTES = 0xd774, KEY_BYTES = 0x20, KIT_BYTES = 0x300, DATA_PAD = 16 };
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
static inline int in(u32 a, u32 bytes, u32 lo, u32 hi) { return !(a & 3) && a >= lo && a < hi && bytes <= hi - a; }
static u32 words_read;                                   // sample words read this frame (the budget, for the tests)
static u32 sample_rd(u32 a) { ++words_read; return rd32(a); }

// The current slot's sample, every address checked. Answers 0 when it is usable, or why not.
enum Why { OK = 0, NO_SLOT, NO_OBJECT, NO_SAMPLE, MONO, BAD_STEREO, TOO_LONG };
static const char* const WHY_TEXT[] = { "", "NO DRUM SLOT", "NO SAMPLE OBJECT", "NO SAMPLE", "MONO: NOT IN V1", "UNKNOWN KIND", "TOO LONG" };
struct Where { u32 slot, obj, data, words, stereo, frames, table; };
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
    w.data = rd32(w.obj);
    w.words = rd32(w.obj + OFF_WORDS);
    w.stereo = rd8(w.obj + OFF_STEREO);
    if (w.stereo > 1) return BAD_STEREO;
    if (w.words < 8u + 2u * 4410u) return NO_SAMPLE;                 // under 0.1 s: nothing to chop
    if (!w.stereo) return MONO;
    w.frames = (w.words - 8u) >> 1;
    if (w.frames > (u32)CAPACITY) return TOO_LONG;
    if (!in(w.data, DATA_PAD + w.frames * 4u, DDR_LO, DDR_HI)) return NO_SAMPLE;
    return OK;
}

// ---------------------------------------------------------------- what the other threads hand over (single words)
static volatile u32 shown_where, shown_how;
static volatile u32 struck;                              // (count << 8) | the keyboard key's number, of the last key down
static volatile int turned[4], turned_shift[4];          // turns, 1024 a step, summed
static volatile u32 pressed[4], pressed_shift[4];        // presses counted
static volatile u32 shift_held;                          // v3: shift (key 38) down, seen by its own down and up
static volatile u32 press4_how;                          // v3: (count << 8) | how the last press of the 4th knob saw shift

// The drum's page is up: shown(OP1F_PLACE_DRUM + slot, OP1F_SHOWN_UP).
static inline int page_up() {
    const u32 w = shown_where;
    return (shown_how & (u32)OP1F_SHOWN_UP) && w >= (u32)OP1F_PLACE_DRUM && w < (u32)OP1F_PLACE_DRUM + (u32)SLOTS;
}

extern "C" void veffchop_shown(unsigned where, unsigned how) OP1F_NAMED("veffchop_shown");
extern "C" void veffchop_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

// The keyboard's keys (13 to 53 but shift, 38) are let through so the kit plays them; their number is noted for the
// highlight. They may come on the audio thread: one word written, nothing else. The knobs and their clicks are taken.
enum { KEYBOARD_LO = 13, KEYBOARD_HI = 53, SHIFT_KEY = 38 };
extern "C" int veffchop_input(const Op1fInput* e) OP1F_NAMED("veffchop_input");
extern "C" int veffchop_input(const Op1fInput* e) {
    if (e->bytes < 36u || !e->mine || !page_up()) return 0;
    if (e->what != (u32)OP1F_TURN && e->key == (u32)SHIFT_KEY) {    // v3: shift tracked by its own down and up
        if (e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) shift_held = 1;
        else if (e->what == (u32)OP1F_UP) shift_held = 0;
        return 0;                                        // the unit sees it as always
    }
    if (e->what != (u32)OP1F_TURN && e->key >= (u32)KEYBOARD_LO && e->key <= (u32)KEYBOARD_HI) {
        if (e->key != (u32)SHIFT_KEY && (e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT))
            struck = (((struck >> 8) + 1u) << 8) | (e->key & 0xffu);
        return 0;                                        // the kit plays it
    }
    const int shift = e->shift || e->what == (u32)OP1F_DOWN_SHIFT || shift_held;
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
static Analysis an;
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

static void knobs() {
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
    targets(sl, key_slice, st.orig[s], target);
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
    const int same = w.slot == st.at.slot && w.obj == st.at.obj && w.data == st.at.data && w.words == st.at.words;
    if (why != OK) {
        st.at.slot = w.slot; st.at.obj = w.obj; st.at.data = w.data; st.at.words = w.words;
        refuse((u32)why);
        return;
    }
    if (!same) {
        st.at.slot = w.slot; st.at.obj = w.obj; st.at.data = w.data; st.at.words = w.words;
        st.at.stereo = w.stereo; st.at.frames = w.frames; st.at.table = w.table;
        st.state = STEADYING; st.seen_since = ms; st.seen_checks = 0; st.why_logged = 0;
        return;
    }
    if (st.state == REFUSED) { st.state = STEADYING; st.seen_since = ms; st.seen_checks = 0; }
    if (st.state == STEADYING) {
        ++st.seen_checks;
        if (ms - st.seen_since < (u32)STEADY_MS || st.seen_checks < 2) return;
        begin(an, st.at.frames);
        st.state = ANALYSING; st.analyse_frames = 0;
        op1f_log(3, LOG_FILE, 6, "veffchop: slot %u: %u frames, stereo; analysing %u windows, %u words a frame",
                 st.at.slot + 1, st.at.frames, an.windows, (u32)BUDGET);
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

static void paint(void* c, u32 ms, int shift) {
    op1f_paint_box(c, 0, 0, 480, 224, OP1F_COLOUR(0, 0, 0));
    char tag[32];
    tag[0] = 0;
    const int ready = st.state == READY;
    if (!page_up()) put(tag, 0, "DRUM ONLY");
    else if (st.note && ms - st.note_at < (u32)NOTE_MS) put(tag, 0, NOTE_TEXT[st.note]);
    else if (st.state == FINISHING) put(tag, 0, "ANALYSING 100%");
    else if (st.state == ANALYSING) put(tag, put_num(tag, put(tag, 0, "ANALYSING "), progress(an)), "%");
    else if (st.state == STEADYING) put(tag, 0, "WAITING");
    else if (st.state == REFUSED) put(tag, 0, WHY_TEXT[st.why]);
    else if (ready) put(tag, put_num(tag, 0, sl.n), sl.n == 1 ? " SLICE" : " SLICES");
    if (shift && tag[0] == 0) put(tag, 0, "SHIFT");

    // the columns
    char v0[16], v1[16], v2[16], v3[16];
    veffui::Column col[4];
    if (shift) {
        put(v0, put_num(v0, 0, st.p.offset_ms), " MS");
        put(v1, put_num(v1, 0, st.p.gap_ms), " MS");
        put(v2, 0, "-");
        put(v3, 0, "PRESS");
        col[0].label = "OFFSET"; col[0].value = v0; col[0].frac = (int)udiv(st.p.offset_ms * 1024u, 20u);
        col[1].label = "GAP"; col[1].value = v1; col[1].frac = (int)udiv((st.p.gap_ms - 20u) * 1024u, 480u);
        col[2].label = ""; col[2].value = v2; col[2].frac = 0;
        col[3].label = "UNDO"; col[3].value = v3; col[3].frac = st.orig_valid[st.at.slot & 7] ? 1024 : 0;
    } else {
        put(v0, 0, MODE_TEXT[st.p.mode]);
        if (st.p.mode == EQUAL) put_num(v1, 0, st.p.parts); else put_num(v1, 0, st.p.sens);
        put(v2, 0, MAP_TEXT[st.p.map]);
        put(v3, 0, st.applied && st.written_valid[st.at.slot & 7] ? "AGAIN" : "PRESS");
        col[0].label = "MODE"; col[0].value = v0; col[0].frac = (int)st.p.mode * 512;
        col[1].label = st.p.mode == EQUAL ? "PARTS" : "SENS"; col[1].value = v1;
        col[1].frac = st.p.mode == EQUAL ? (int)udiv((st.p.parts - 2u) * 1024u, 22u) : (int)udiv(st.p.sens * 1024u, 100u);
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
        if (st.state == REFUSED && st.why == MONO) veffui::text(c, WX, 96, "V1 CHOPS STEREO SAMPLES ONLY", VEFF_GREY, 2, 2);
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
    if (shown_where != st.shown_where_seen || shown_how != st.shown_how_seen) {
        st.shown_where_seen = shown_where; st.shown_how_seen = shown_how;
        op1f_log(3, LOG_FILE, 8, "veffchop: drum page %s (where %u, how %u)", page_up() ? "up" : "off", st.shown_where_seen, st.shown_how_seen);
    }
    knobs();
    const u32 was = st.state;
    watch(now->ms);
    if (st.state == READY && was == READY && st.dirty) {    // v2: never in the frame the analysis ends
        st.dirty = 0;
        slice(an, st.p, sl);
        map(an, sl, st.p.map, key_slice);
    }
    for (int i = 0; i < 4; ++i) {                        // presses: the fourth applies, shift and it undoes
        const u32 p = pressed[i], ps = pressed_shift[i];
        const int hit = p != st.press_seen[i], hit_s = ps != st.press_shift_seen[i];
        st.press_seen[i] = p; st.press_shift_seen[i] = ps;
        if (i == 3 && hit && page_up()) apply(now->ms);
        if (i == 3 && hit_s && page_up()) undo(now->ms);
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
