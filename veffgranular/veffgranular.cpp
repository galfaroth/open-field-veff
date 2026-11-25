// veffgranular: a granular sample synth engine for the OP-1 Field (open-field level 9/10, OS 1.7.3+0).
//
//   `veffgranular` in the unit's engine list (shift and T1 on a synth sound). Each key plays grains of one sample,
//   pitched from the note against the sample's root. Three modes: classic (grains at the position), loop (grains
//   follow a head looping a short region, free or a tempo length) and cloud (randomised dense grains).
//   Knobs: position, size, density, spray; with shift: scan, shape, tune, reverse. Those are the eight engine
//   values, kept with the sound by the unit and moved by its LFOs (U_* below).
//   Shift and a press of the 1st knob steps to the second shift page, B: spread, volume, pitch random, level random.
//   These four, with the mode, the loop length and the file number, are kept with the sound through level 7
//   (`veffgranular_set`); the pack takes the knobs' turns for them itself.
//   Presses: 1st mode, 2nd shape back to 0, 3rd loop length, 4th the next file. With shift: 1st the other shift page;
//   2nd, 3rd, 4th put the page's value back (A: shape, tune, reverse; B: volume, pitch random, level random).
//
// Against version 1, values 6 and 8 changed their meaning (they were spread and volume). A sound saved by version 1
// is told by its level-7 record ("v":1), or, when it has none, by values 6 and 8 standing exactly where version 1's
// starts put them; its spread and volume are then moved to their new place and shape and reverse set to 0.
//
// Where the sample comes from (grain_source.h is the interface; source_for() below is the one place that chooses):
//   1. the sampler source (the default; a file `sampler.off` in the pack's folder switches it off): the sample the
//      unit's synth sampler holds for this sound's slot, read in place, OS 1.7.3+0 only. Where it lies was read in
//      the firmware on a computer and confirmed by a probe pack on one unit (sampler_region.h). When its checks fail
//      (another firmware, an address that does not look right, an empty slot) it is not valid and the file source
//      plays instead;
//   2. the file source: a WAV or AIFF named 1.wav .. 8.aif in the pack's folder `veffgranular` (disk mode), read
//      into 4 MiB of working memory (level 6) by the frame function.
// The DSP is granular_core.h, which calls nothing of the unit's; tests/host_test.cpp runs it on a computer.

#include "build_r6e_card.h"
// Everything but the render is built for size (the unit's room for packs is short): the page, the files, the keys.
#define VEFF_SMALL _Pragma("GCC push_options") _Pragma("GCC optimize(\"Os\")")
#define VEFF_FAST _Pragma("GCC pop_options")
VEFF_SMALL
#include "veff_ui.h"
VEFF_FAST
#include "granular_core.h"
VEFF_SMALL
#include "sample_file.h"
#include "sampler_region.h"
#include "sampler_scan.h"
#include "drop_logic.h"

typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;

extern "C" const Op1fCard op1f_card_veffgranular OP1F_NAMED("op1f_card_veffgranular");
#define CARD (&op1f_card_veffgranular)

enum { ASK_ENGINE = 0, ASK_MEMORY = 1, ASK_KEEPS = 2, FOLDER = 0 };
enum { BLOCK_LEVEL_9 = 52, FRAME_LEVEL_4 = 40, INPUT_LEVEL_2 = 36 };
enum { CLICK_FIRST = 54, CLICK_LAST = 57, PRESS_FIRST = 61, PRESS_LAST = 64 };
enum { SLOTS = 8, VOICES = 6, FILES = 8, MEMORY_BYTES = 4194304, NAMES_ROOM = 1024 };
static const char LOG_FILE[] = "op1f/veffgranular.cpp";
enum { U_POS, U_SIZE, U_DENSITY, U_SPRAY, U_SCAN, U_SHAPE, U_TUNE, U_REVERSE };   // the engine's eight values
static const int VALUE_START[8] = { 250, 50, 50, 10, 0, 0, 0, 0 };     // as the recipe's starts
enum { X_SPREAD, X_VOLUME, X_PITCH_RANDOM, X_LEVEL_RANDOM, EXTRAS };   // page B, kept through level 7
static const int EXTRA_START[EXTRAS] = { 30, 80, 0, 0 };

// ---------------------------------------------------------------- shared between the threads (single words)
static volatile u32 set_mode[SLOTS], set_loop[SLOTS], set_file[SLOTS];   // kept values
static volatile int set_extra[SLOTS][EXTRAS];         // spread, volume, pitch random, level random: 0..100
static volatile u32 shift_page;                       // 0: A (the unit's values), 1: B (ours)
static volatile u32 playing_slot;                     // the slot the render last played, + 1 (0: none yet)
static volatile u32 file_loaded;                      // the file number in file_src, 1..8, or 0
static volatile u32 sampler_on = 1;                   // the sampler source is on unless a file sampler.off says no
static GrainSource file_src;
static GrainSource sampler_src[SLOTS];
static GrainSource none_src;                          // valid 0
static u32 generation_next = 1;                       // display thread only
static gran::Voice voices[VOICES];                    // the audio thread only
static gran::Vis vis[VOICES];                         // the render writes, the page reads

// ================================================================ THE SAMPLER SOURCE plugs in here
// source_for(slot): the sample a voice of synth sound `slot` (0..7) plays.
//   Called on the AUDIO thread, once at the start of every render; the render copies what it returns and uses only
//   the copy for that call. It must return quickly, call nothing, and point at a GrainSource that obeys
//   grain_source.h's rules: `valid` 0 while anything behind it is being changed, a new `generation` whenever the
//   frames, the region or the format change, frames read only within [start, end).
//   For the sampler: data = the slot's 16-bit pool, frames/stereo from the sampler's object, start/end = the
//   sampler's start and end points (today: the whole buffer past its head pad, the points are not found yet),
//   root = the sampler's root (today: 60), rate_q16 = 65536 (44.1 kHz). sampler_refresh() fills it on the display
//   thread.
static volatile u32 drop_key = dropx::KEY_DEFAULT;      // the drop key's number (dropkey.txt may give another)
static volatile u32 drop_hold;                        // a bit a slot: silenced while a drop of ours rewrites its pool
static const GrainSource* source_for(u32 slot) {
    if (drop_hold >> slot & 1u) return &none_src;
    if (sampler_on && sampler_src[slot].valid) return &sampler_src[slot];
    if (file_loaded && set_file[slot] == file_loaded) return &file_src;
    return &none_src;
}
// ================================================================

// a source changed: write it so a render in between sees either nothing (valid 0) or the whole new one
static void source_publish(GrainSource* s, const short* data, u32 frames, u32 stereo, u32 start, u32 end, u32 root, u32 rate_q16) {
    s->valid = 0;
    __asm__ __volatile__("" ::: "memory");
    s->data = data; s->frames = frames; s->stereo = stereo; s->start = start; s->end = end; s->root = root;
    s->rate_q16 = rate_q16; s->generation = generation_next++;
    __asm__ __volatile__("" ::: "memory");
    s->valid = 1;
}

// ---------------------------------------------------------------- the engine (audio thread)
VEFF_FAST
extern "C" void veffgranular_render(const Op1fVoiceBlock* now) OP1F_NAMED("veffgranular_render");
extern "C" void veffgranular_render(const Op1fVoiceBlock* now) {
    int* out = now->out;
    if (now->bytes < (u32)BLOCK_LEVEL_9 || now->voice >= (u32)VOICES) {
        for (u32 k = 0; k < 2u * gran::FRAMES; ++k) out[k] = 0;
        return;
    }
    const u32 slot = now->slot & 7u;
    playing_slot = slot + 1u;
    const volatile GrainSource* from = source_for(slot);
    GrainSource s;                                    // field by field: the module links no memcpy
    s.valid = from->valid;
    s.data = from->data; s.frames = from->frames; s.stereo = from->stereo; s.start = from->start; s.end = from->end;
    s.root = from->root; s.rate_q16 = from->rate_q16; s.generation = from->generation;
    // the render's eight numbers: six of the unit's, and spread and volume from page B
    const int* u = now->values;
    int vals[gran::VALUES];
    vals[gran::V_POS] = u[U_POS]; vals[gran::V_SIZE] = u[U_SIZE]; vals[gran::V_DENSITY] = u[U_DENSITY]; vals[gran::V_SPRAY] = u[U_SPRAY];
    vals[gran::V_SCAN] = u[U_SCAN]; vals[gran::V_TUNE] = u[U_TUNE];
    vals[gran::V_SPREAD] = set_extra[slot][X_SPREAD]; vals[gran::V_VOLUME] = set_extra[slot][X_VOLUME];
    gran::Settings set;
    set.mode = set_mode[slot]; set.loop = set_loop[slot];
    set.shape = u[U_SHAPE]; set.reverse = (u32)(u[U_REVERSE] < 0 ? 0 : u[U_REVERSE]);
    set.pitch_random = (u32)set_extra[slot][X_PITCH_RANDOM]; set.level_random = (u32)set_extra[slot][X_LEVEL_RANDOM];
    gran::Block b;
    b.values = vals; b.step = now->step; b.period = now->period; b.voice = now->voice; b.out = out;
    b.events = ((now->events & (u32)OP1F_VOICE_BEGINS) ? (u32)gran::EV_BEGINS : 0u) |
               ((now->events & (u32)OP1F_VOICE_ENDS) ? (u32)gran::EV_ENDS : 0u) |
               ((now->events & (u32)OP1F_VOICE_FRESH) ? (u32)gran::EV_FRESH : 0u);
    gran::render(voices[now->voice], s, set, b, &vis[now->voice]);
}
VEFF_SMALL

static volatile u32 shown_where, shown_how;
extern "C" void veffgranular_shown(unsigned where, unsigned how) OP1F_NAMED("veffgranular_shown");
extern "C" void veffgranular_shown(unsigned where, unsigned how) { shown_where = where; shown_how = how; }

// ---------------------------------------------------------------- small helpers (display thread)
static char* put(char* p, const char* s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char* put_number(char* p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char d[12]; int k = 0; u32 n = (u32)v;
    do { u32 tens = 0; while (n >= 10u) { n -= 10u; ++tens; } d[k++] = (char)('0' + n); n = tens; } while (n && k < 11);
    while (k) *p++ = d[--k];
    *p = 0; return p;
}
static char* put_tenths(char* p, int v) {           // 253 -> 25.3
    if (v < 0) { *p++ = '-'; v = -v; }
    int whole = 0; while (v >= 10) { v -= 10; ++whole; }
    p = put_number(p, whole); *p++ = '.'; *p++ = (char)('0' + v); *p = 0; return p;
}
static inline int held(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// (a << 32) / b for a < b, by shifts
static u32 frac32(u32 a, u32 b) {
    u32 q = 0, r = a;
    for (int bit = 31; bit >= 0; --bit) { const u32 top = r >> 31; r <<= 1; if (top || r >= b) { r -= b; q |= 1u << bit; } }
    return q;
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ends_with(const char* s, const char* tail) {
    int n = 0, m = 0; while (s[n]) ++n; while (tail[m]) ++m;
    if (m > n) return 0;
    for (int i = 0; i < m; ++i) if (lower(s[n - m + i]) != tail[i]) return 0;
    return 1;
}
static int same_name(const char* a, const char* b) { while (*a && lower(*a) == *b) { ++a; ++b; } return !*a && !*b; }

// ---------------------------------------------------------------- the file source (display thread)
static struct {
    u32 listed, want, tried, error, frames, stereo, rate;  // error: sfile's number, 100 + no file, 101 no memory, 102 too large
    char name[40];
} files;
static char names[NAMES_ROOM];

static int list_folder() {                            // answers the names' count, notes sampler.off
    const int n = op1f_list(CARD, FOLDER, names, sizeof names);
    u32 on = 1;
    const char* p = names;
    for (int i = 0; i < n; ++i) { if (same_name(p, "sampler.off")) on = 0; while (*p) ++p; ++p; }
    if (on != sampler_on) op1f_log(3, LOG_FILE, 1, "veffgranular: sampler source %u", on);
    sampler_on = on;
    {   // the drop key's number, when a file dropkey.txt gives another than the one built in (DROP.md)
        char t[16];
        const int got = op1f_read(CARD, FOLDER, "dropkey.txt", t, sizeof t);
        const u32 key = got > 0 ? dropx::key_from_text(t, (u32)got) : (u32)dropx::KEY_DEFAULT;
        if (key != drop_key) op1f_log(3, LOG_FILE, 15, "veffgranular: drop key %u", key);
        drop_key = key;
    }
    return n;
}

static const char* find_file(u32 number, int count) {  // "3.wav", "3 pad.aif": the first that starts with the digit
    const char* p = names;
    for (int i = 0; i < count; ++i) {
        const char* name = p;
        while (*p) ++p;
        ++p;
        if (name[0] != (char)('0' + number) || (name[1] >= '0' && name[1] <= '9')) continue;
        if (ends_with(name, ".wav") || ends_with(name, ".aif") || ends_with(name, ".aiff") || ends_with(name, ".aifc")) return name;
    }
    return 0;
}

static void file_load(u32 number) {
    files.tried = number;
    files.name[0] = 0;
    file_loaded = 0;
    file_src.valid = 0;                               // from here no render reads the buffer
    u8* mem = (u8*)op1f_memory(CARD, ASK_MEMORY);
    if (!mem) { files.error = 101; return; }
    const int count = list_folder();
    const char* name = count > 0 ? find_file(number, count) : 0;
    if (!name) { files.error = 100; return; }
    int i = 0; for (; name[i] && i < 39; ++i) files.name[i] = name[i]; files.name[i] = 0;
    const int got = op1f_read(CARD, FOLDER, name, mem, (unsigned)MEMORY_BYTES);
    if (got == OP1F_TOO_LARGE) { files.error = 102; op1f_log(2, LOG_FILE, 2, "veffgranular: %s is over 4 MiB", files.name); return; }
    if (got < 0) { files.error = 100; return; }
    sfile::Info in;
    const int e = sfile::parse(mem, (u32)got, &in);
    files.error = (u32)e;
    if (e != sfile::OK) { op1f_log(2, LOG_FILE, 3, "veffgranular: %s not taken: %s", files.name, sfile::ERROR_TEXT[e]); return; }
    files.frames = in.frames; files.stereo = in.stereo; files.rate = in.rate;
    source_publish(&file_src, (const short*)mem, in.frames, in.stereo, 0, in.frames, in.root, in.rate_q16);
    file_loaded = number;
    op1f_log(3, LOG_FILE, 4, "veffgranular: %s: %u frames, st%u, %u Hz, root %u", files.name, in.frames, in.stereo, in.rate, in.root);
}

// ---------------------------------------------------------------- the sampler source (display thread)
// OS 1.7.3+0 only. What our code needs of the firmware (read on a computer, then confirmed by a probe pack on one
// unit): a table of eight words, one a synth slot, each pointing at the slot's sample holder; holder + 0 the sample
// object; object + 0 the 16-bit data (the slot's fixed pool), + 0xd768 the frames it holds at most, + 0xd76c the
// 16-bit words the last load, drop or recording wrote (0: an empty slot), byte + 0xd770 stereo. The first 16 bytes
// of the data are a zero pad. Only read, never written. sampler_region.h turns the three fields into the region.
static const u32 SLOT_TABLE = 0x0802f73cu;
static const u32 POOL = 0x840b0becu, POOL_STRIDE = 0x102660u;    // a slot's pool: 264,600 stereo frames
static const u32 DDR_LO = 0x80000000u, DDR_HI = 0x90000000u;
enum { OFF_FRAMES = 0xd768, OFF_WORDS = 0xd76c, OFF_STEREO = 0xd770, OBJECT_BYTES = 0xd774, POOL_FRAMES_STEREO = 264600, POOL_FRAMES_MONO = 529200 };
static inline u32 rd32(u32 a) { return *(const volatile u32*)a; }
static inline u32 rd8(u32 a) { return *(const volatile u8*)a; }
static inline int in_ddr(u32 a, u32 bytes) { return a >= DDR_LO && a < DDR_HI && bytes <= DDR_HI - a; }

struct SamplerSeen { u32 data, frames, words, stereo, sum, first, end, bad; };
// Never assigned or initialised as a whole: the compiler would call memcpy or memset for it, and the unit has none
// (a pack that needs such a name is left out at the start). Word by word, through these two.
static void seen_copy(SamplerSeen& to, const SamplerSeen& from) {
    to.data = from.data; to.frames = from.frames; to.words = from.words; to.stereo = from.stereo;
    to.sum = from.sum; to.first = from.first; to.end = from.end; to.bad = from.bad;
}
static void seen_clear(SamplerSeen& s) { s.data = s.frames = s.words = s.stereo = s.sum = s.first = s.end = s.bad = 0; }
static SamplerSeen sampler_seen[SLOTS];

// A cheap sum of a pool: 8 runs of 8 words spread over `bytes` of it, and 12 single words at 64, 128 .. 131,072
// bytes (so that a short sound is seen too): 76 words, all inside the pool whatever `bytes` is. With no data (0, 0)
// it answers what a silent pool sums to.
static u32 pool_sum(u32 data, u32 bytes) {
    u32 sum = 0x9e3779b9u;
    const u32 step = (bytes >> 3) & ~3u;
    for (u32 k = 0; k < 8u; ++k) for (u32 i = 0; i < 8u; ++i) sum = (sum ^ (data ? rd32(data + k * step + i * 4u) : 0u)) * 0x01000193u;
    for (u32 n = 0; n < 12u; ++n) sum = (sum ^ (data ? rd32(data + (64u << n)) : 0u)) * 0x01000193u;
    return sum;
}

static u32 own_frames[SLOTS], own_sum[SLOTS], own_summed[SLOTS];   // a drop of our own: its frames, and the pool's sum after it
// Where a slot's sound ends is searched a little each frame (sampler_scan.h), one slot at a time, and only after the
// slot's cheap sum (8 runs of 8 words) or its fields changed. Until its search is over a slot plays and shows nothing.
static SamplerSeen scan_want[SLOTS];                  // the slot as it was when its search was asked for
static sscan::Scan scan;
static u32 scan_slot = SLOTS, scan_pending, scan_frames, scans_done;   // scan_slot SLOTS: none under way
static u32 words_this_frame, words_most;              // what the frame read of the pools, and the most any frame did

static void sampler_found(u32 slot, SamplerSeen& s, u32 first, u32 end) {   // where the sample begins and ends is known
    SamplerSeen& was = sampler_seen[slot];
    s.first = first; s.end = end;
    if (end < first + 2u) {
        s.bad = SAMPLER_EMPTY;
        if (was.bad != s.bad) op1f_log(3, LOG_FILE, 5, "veffgranular: synth %u: no sound in the sampler (f%u st%u)", slot + 1, s.frames, s.stereo);
        sampler_src[slot].valid = 0;
    } else {
        source_publish(&sampler_src[slot], (const short*)s.data, end, s.stereo, first, end, 60, 65536);
        op1f_log(3, LOG_FILE, 6, "veffgranular: synth %u: sampler frames %u..%u, f%u w%u st%u, ours %u", slot + 1, first, end, s.frames, s.words, s.stereo, own_frames[slot]);
    }
    seen_copy(was, s);
}

// The cheap look at a slot: its fields and the sum, 64 words. Asks for a search when they changed.
static void sampler_refresh(u32 slot) {
    SamplerSeen s;
    seen_clear(s);
    const u32 holder = rd32(SLOT_TABLE + slot * 4u);
    if ((holder & 3u) || !in_ddr(holder, 8)) s.bad = 1;
    u32 object = 0;
    if (!s.bad) { object = rd32(holder); if ((object & 3u) || !in_ddr(object, OBJECT_BYTES)) s.bad = 2; }
    if (!s.bad) {
        s.data = rd32(object);
        s.frames = rd32(object + OFF_FRAMES);
        s.words = rd32(object + OFF_WORDS);
        s.stereo = rd8(object + OFF_STEREO);
        if (s.data != POOL + slot * POOL_STRIDE) s.bad = 3;
        else if (s.stereo > 1u) s.bad = SAMPLER_BAD_STEREO;
    }
    SamplerSeen& was = sampler_seen[slot];
    const u32 bit = 1u << slot;
    if (s.bad) {
        if (was.bad != s.bad) op1f_log(2, LOG_FILE, 5, "veffgranular: synth %u: sampler source not valid (check %u)", slot + 1, s.bad);
        sampler_src[slot].valid = 0;
        scan_pending &= ~bit; if (scan_slot == slot) scan_slot = SLOTS;
        seen_copy(was, s);
        return;
    }
    s.sum = pool_sum(s.data, sampler_capacity(s.frames, s.stereo) << (s.stereo ? 2 : 1));
    words_this_frame += 76u;
    const SamplerSeen& ref = (scan_pending & bit) ? scan_want[slot] : was;   // what the last look saw
    const int looked = (scan_pending & bit) || was.bad == 0u || was.bad == (u32)SAMPLER_EMPTY;
    if (looked && ref.data == s.data && ref.frames == s.frames && ref.stereo == s.stereo && ref.sum == s.sum) return;
    // changed, or first seen
    if (own_frames[slot] && own_summed[slot] && own_sum[slot] != s.sum) own_frames[slot] = 0;   // no longer as our drop left it
    if (own_frames[slot]) {                           // a drop of our own: its frames are known, nothing to search
        if (!own_summed[slot]) { own_sum[slot] = s.sum; own_summed[slot] = 1; }
        scan_pending &= ~bit; if (scan_slot == slot) scan_slot = SLOTS;
        const u32 capacity = sampler_capacity(s.frames, s.stereo);
        sampler_found(slot, s, 0, own_frames[slot] < capacity ? own_frames[slot] : capacity);
        return;
    }
    if (s.words || s.sum == pool_sum(0, 0)) {         // loaded from a file: `words` says where it ends; or a silent pool
        u32 first = 0, end = 0;
        if (s.words) sampler_region_loaded(s.frames, s.words, s.stereo, &first, &end);
        scan_pending &= ~bit; if (scan_slot == slot) scan_slot = SLOTS;
        sampler_found(slot, s, first, end);
        return;
    }
    sampler_src[slot].valid = 0;                      // nothing of it is played or shown until the search is over
    seen_copy(scan_want[slot], s);
    scan_pending |= bit;
    if (scan_slot == slot) scan_slot = SLOTS;         // a search under way was for what the pool held before
}

// One frame of the search: at most sscan::BUDGET_WORDS words of one slot's pool.
static void sampler_scan_frame() {
    if (scan_slot >= (u32)SLOTS) {
        if (!scan_pending) return;
        u32 slot = 0; while (!(scan_pending >> slot & 1u)) ++slot;
        scan_slot = slot; scan_frames = 0;
        sscan::begin(scan, sampler_capacity(scan_want[slot].frames, scan_want[slot].stereo), scan_want[slot].stereo);
    }
    ++scan_frames;
    words_this_frame += sscan::step(scan, (const volatile short*)scan_want[scan_slot].data, (unsigned)sscan::BUDGET_WORDS);
    if (scan.state != (unsigned)sscan::DONE) return;
    const u32 slot = scan_slot;
    scan_slot = SLOTS; scan_pending &= ~(1u << slot);
    SamplerSeen s;
    seen_copy(s, scan_want[slot]);
    sampler_found(slot, s, 0, scan.e);
    if (!scans_done++) op1f_log(3, LOG_FILE, 18, "veffgranular: v6 first scan done: synth %u, end %u, %u frame(s)", slot + 1, scan.e, scan_frames);
}

// the slot's sample object when its table word, holder, object and pool are where the sampler source expects them; 0
static u32 sampler_object(u32 slot) {
    const u32 holder = rd32(SLOT_TABLE + slot * 4u);
    if ((holder & 3u) || !in_ddr(holder, 8)) return 0;
    const u32 object = rd32(holder);
    if ((object & 3u) || !in_ddr(object, OBJECT_BYTES)) return 0;
    if (rd32(object) != POOL + slot * POOL_STRIDE) return 0;
    return object;
}

// ---------------------------------------------------------------- THE DROP EXPERIMENT (version 3; DROP.md)
// Not for the listing. On the drop key, while the page is up, the firmware's own import of the tape clipboard into a
// sample is called for this slot's sample object, as the sampler's worker does for a sampler sound, from the frame
// function (the display thread). OS 1.7.3+0 only: nothing is called unless the firmware's bytes at the worker's call
// site sum to what that firmware's do. Read in the firmware on a computer; what a unit does is not known.
static const u32 DROP_SITE = 0x8f00b838u, DROP_SITE_BYTES = 14u, DROP_SITE_SUM = 0x64cae65eu;   // FNV-1a of the worker's two calls
typedef int (*DropImport)(void* sample);
#define STOCK_IMPORT ((DropImport)0x8f004204u)        // its entry: where another pack's wrapper for tracks 5 to 8 sits too
static const u32 CLIP_BEGIN = 0x84bdd948u, CLIP_END = 0x84bdd94cu, CLIP_LENGTH = 0x84bdd9e4u, CLIP_OTHER = 0x84bdda00u;
static const u32 WORKER_READ = 0x84bdd8a8u + 0x88u, WORKER_WRITE = 0x84bdd8a8u + 0x8cu;
static dropx::Machine drop;                           // the display thread only
static volatile u32 drop_request, drop_armed, drop_busy;
static u32 drop_site_checked, drop_after, drop_after_slot, drop_after_sum;

static void drop_facts(dropx::Facts& f, u32 slot, u32 ours, u32 disk) {
    f.site_ok = drop_armed; f.ours = ours; f.disk = disk;
    f.chain_ok = 0; f.clip_length = f.clip_other = f.clip_tracks = 0; f.queue_read = 0; f.queue_write = 1; f.frames = 0; f.stereo = 0;
    if (!drop_armed) return;                          // another firmware: none of its words is read
    const u32 object = slot < (u32)SLOTS ? sampler_object(slot) : 0u;
    f.chain_ok = object != 0;
    if (object) { f.frames = rd32(object + OFF_FRAMES); f.stereo = rd8(object + OFF_STEREO); }
    f.clip_length = rd32(CLIP_LENGTH); f.clip_other = rd32(CLIP_OTHER);
    f.clip_tracks = (rd32(CLIP_END) - rd32(CLIP_BEGIN)) >> 2;
    f.queue_read = rd32(WORKER_READ); f.queue_write = rd32(WORKER_WRITE);
}
static void drop_frame(const Op1fFrame* now, u32 how, u32 where, u32 riding) {
    if (!drop_site_checked) {
        drop_site_checked = 1;
        u32 sum = 0x811c9dc5u;
        for (u32 i = 0; i < DROP_SITE_BYTES; ++i) sum = (sum ^ rd8(DROP_SITE + i)) * 0x01000193u;
        drop_armed = sum == DROP_SITE_SUM;
        op1f_log(3, LOG_FILE, 11, "veffgranular: drop: site %08x, armed %u, key %u", sum, (u32)drop_armed, (u32)drop_key);
    }
    const u32 request = drop_request;
    if (request) drop_request = 0;
    const u32 slot = drop.state == (u32)dropx::IDLE ? (request ? request - 1u : where) : drop.slot;
    const u32 ours = (how & (u32)OP1F_SHOWN_UP) && slot == where && slot < (u32)SLOTS && (riding >> slot & 1u);
    dropx::Facts f;
    drop_facts(f, slot, ours, now->disk);
    const u32 refused_before = drop.refused;
    const int action = dropx::step(drop, request, f);
    if (drop.refused != refused_before)
        op1f_log(2, LOG_FILE, 12, "veffgranular: drop refused %d: synth %u clip %u t%u q%u/%u; %u done %u refused",
                 drop.reason, slot + 1, f.clip_length, f.clip_tracks, f.queue_read, f.queue_write, drop.done, drop.refused);
    if (action == dropx::A_HOLD) { drop_hold |= 1u << drop.slot; sampler_src[drop.slot].valid = 0; }
    else if (action == dropx::A_RELEASE) drop_hold &= ~(1u << drop.slot);
    else if (action == dropx::A_CALL) {
        const u32 object = sampler_object(drop.slot);            // not 0: the checks held this frame
        const u32 slot_now = drop.slot;
        u32 frames = rd32(object + OFF_FRAMES), stereo = rd8(object + OFF_STEREO);
        const u32 bytes = sampler_capacity(frames, stereo > 1u ? 1u : stereo) << (stereo ? 2 : 1);
        const u32 sum_before = pool_sum(rd32(object), bytes);
        op1f_log(3, LOG_FILE, 13, "veffgranular: drop> synth %u obj %08x f%u w%u st%u k%u x %08x %08x %08x sum %08x clip %u t%u o%u q%u/%u",
                 slot_now + 1, object, frames, rd32(object + OFF_WORDS), stereo, rd8(object + OFF_STEREO + 1u),
                 rd32(object + 0xd75cu), rd32(object + 0xd760u), rd32(object + 0xd764u), sum_before,
                 f.clip_length, f.clip_tracks, f.clip_other, f.queue_read, f.queue_write);
        const int answer = STOCK_IMPORT((void*)object);
        dropx::called(drop);
        frames = rd32(object + OFF_FRAMES); stereo = rd8(object + OFF_STEREO);
        const u32 sum_after = pool_sum(rd32(object), sampler_capacity(frames, stereo > 1u ? 1u : stereo) << (stereo ? 2 : 1));
        // what the drop rendered: the clipboard's frames, as many as the sample may hold (the import's own rule)
        const u32 capacity = sampler_capacity(frames, stereo > 1u ? 1u : stereo);
        if (sum_after != sum_before) { own_frames[slot_now] = f.clip_length < capacity ? f.clip_length : capacity; own_summed[slot_now] = 0; }
        else if (!(own_frames[slot_now] && own_summed[slot_now] && own_sum[slot_now] == sum_after)) own_frames[slot_now] = 0;   // the same cut again keeps what is known
        sampler_seen[slot_now].data = 0;                          // seen anew: a new generation, a fresh overview
        sampler_refresh(slot_now);
        drop_hold &= ~(1u << slot_now);
        op1f_log(3, LOG_FILE, 14, "veffgranular: drop< %d f%u w%u st%u x %08x %08x %08x sum %08x changed %u ours %u q%u/%u; %u done %u refused",
                 answer, frames, rd32(object + OFF_WORDS), stereo, rd32(object + 0xd75cu), rd32(object + 0xd760u), rd32(object + 0xd764u),
                 sum_after, (u32)(sum_after != sum_before), own_frames[slot_now], rd32(WORKER_READ), rd32(WORKER_WRITE), drop.done, drop.refused);
        drop_after = 40; drop_after_slot = slot_now; drop_after_sum = sum_after;
    }
    if (drop_after && --drop_after == 0 && drop_armed) {          // 40 frames later: did anything go on changing?
        const u32 object = sampler_object(drop_after_slot);
        if (object) {
            const u32 frames = rd32(object + OFF_FRAMES), stereo = rd8(object + OFF_STEREO) ? 1u : 0u;
            const u32 sum = pool_sum(rd32(object), sampler_capacity(frames, stereo) << (stereo ? 2 : 1));
            op1f_log(3, LOG_FILE, 16, "veffgranular: drop+40 synth %u w%u sum %08x same %u q%u/%u", drop_after_slot + 1,
                     rd32(object + OFF_WORDS), sum, (u32)(sum == drop_after_sum), rd32(WORKER_READ), rd32(WORKER_WRITE));
        }
    }
    drop_busy = drop.state != (u32)dropx::IDLE;
}

// ---------------------------------------------------------------- the overview, a little each frame
enum { COLS = 216, COL_PER_FRAME = 8, READS_PER_COL = 64 };
static signed char ov_hi[COLS], ov_lo[COLS];
static u32 ov_col, ov_gen;
static void overview_step(const GrainSource& s) {
    if (!grain_source_ok(s)) { ov_gen = 0; return; }
    if (ov_gen != s.generation) { ov_gen = s.generation; ov_col = 0; for (int c = 0; c < COLS; ++c) ov_hi[c] = ov_lo[c] = 0; }
    const u32 len = s.end - s.start, ch = s.stereo ? 2u : 1u;
    const volatile short* d = (const volatile short*)s.data;
    for (int k = 0; k < COL_PER_FRAME; ++k) {
        const u32 c = ov_col;
        const u32 a = s.start + gran::mul_shift(len, c * 19884107u, 32), z = s.start + gran::mul_shift(len, (c + 1u) * 19884107u, 32);
        u32 stride = 1; while (stride * (u32)READS_PER_COL < z - a) stride <<= 1;
        int hi = 0, lo = 0;
        for (u32 f = a; f < z && f < s.end; f += stride) { const int v = d[f * ch]; if (v > hi) hi = v; if (v < lo) lo = v; }
        ov_hi[c] = (signed char)(hi >> 9); ov_lo[c] = (signed char)(lo >> 9);   // +-64, drawn at +-50
        ov_col = c + 1u >= (u32)COLS ? 0u : c + 1u;
    }
}

// ---------------------------------------------------------------- the kept values (level 7)
static const char KEY[] = "veffgranular_set";
static volatile u32 reread;
static u32 ready, unsaved;
extern "C" void veffgranular_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("veffgranular_changed");
extern "C" void veffgranular_changed(unsigned where, unsigned which, unsigned how) {
    (void)how;
    if (where == (unsigned)OP1F_KEEP_SYNTH && which < (unsigned)SLOTS) reread |= 1u << which;
}
static int read_member(const char* t, char name, int* out) {
    for (; *t; ++t) if (t[0] == '"' && t[1] == name && t[2] == '"') {
        const char* q = t + 3; while (*q == ' ' || *q == ':') ++q;
        int v = 0, any = 0; while (*q >= '0' && *q <= '9' && v < 1000) { v = v * 10 + (*q - '0'); ++q; any = 1; }
        if (any) { *out = v; return 1; }
        return 0;
    }
    return 0;
}
static void kept_read(u32 slot) {
    char text[128];
    set_mode[slot] = 0; set_loop[slot] = 0; set_file[slot] = 1;
    for (int e = 0; e < EXTRAS; ++e) set_extra[slot][e] = EXTRA_START[e];
    int version = 0;
    if (op1f_value_get(CARD, OP1F_KEEP_SYNTH, slot, KEY, text, sizeof text) > 0) {
        int v;
        if (read_member(text, 'm', &v)) set_mode[slot] = (u32)held(v, 0, gran::MODES - 1);
        if (read_member(text, 'l', &v)) set_loop[slot] = (u32)held(v, 0, gran::LOOPS - 1);
        if (read_member(text, 'f', &v)) set_file[slot] = (u32)held(v, 1, FILES);
        if (read_member(text, 'd', &v)) set_extra[slot][X_SPREAD] = held(v, 0, 100);
        if (read_member(text, 'o', &v)) set_extra[slot][X_VOLUME] = held(v, 0, 100);
        if (read_member(text, 'p', &v)) set_extra[slot][X_PITCH_RANDOM] = held(v, 0, 100);
        if (read_member(text, 'a', &v)) set_extra[slot][X_LEVEL_RANDOM] = held(v, 0, 100);
        if (read_member(text, 'v', &v)) version = v;
    }
    if (version < 2) {
        // No record of ours, or version 1's. Version 1 kept spread as value 6 (0..100) and volume as value 8: read as
        // this version's shape (-100..100) and reverse (0..100) they stand at 2 * spread - 100 and at the volume.
        int shape = 0, reverse = 0;
        const int got = op1f_param_get(CARD, ASK_ENGINE, slot, U_SHAPE, &shape) == 0 && op1f_param_get(CARD, ASK_ENGINE, slot, U_REVERSE, &reverse) == 0;
        const int v1_starts = shape == -40 && reverse == 80;             // spread 30 and volume 80, version 1's starts
        if (got && (version == 1 || v1_starts)) {
            set_extra[slot][X_SPREAD] = held((shape + 100) >> 1, 0, 100);
            set_extra[slot][X_VOLUME] = held(reverse, 0, 100);
            op1f_param_set(CARD, ASK_ENGINE, slot, U_SHAPE, 0);
            op1f_param_set(CARD, ASK_ENGINE, slot, U_REVERSE, 0);
            op1f_log(3, LOG_FILE, 9, "veffgranular: synth %u: a version 1 sound, spread %d volume %d moved to page B",
                     slot + 1, (int)set_extra[slot][X_SPREAD], (int)set_extra[slot][X_VOLUME]);
        }
        unsaved |= 1u << slot;                                           // from now on the sound carries a record of version 2
    }
    ready |= 1u << slot;
}
static void kept_write(u32 slot) {                    // {"m":0,"l":0,"f":1,"d":30,"o":80,"p":0,"a":0,"v":2}
    char text[96], * q = text;
    q = put(q, "{\"m\":"); q = put_number(q, (int)set_mode[slot]);
    q = put(q, ",\"l\":"); q = put_number(q, (int)set_loop[slot]);
    q = put(q, ",\"f\":"); q = put_number(q, (int)set_file[slot]);
    q = put(q, ",\"d\":"); q = put_number(q, (int)set_extra[slot][X_SPREAD]);
    q = put(q, ",\"o\":"); q = put_number(q, (int)set_extra[slot][X_VOLUME]);
    q = put(q, ",\"p\":"); q = put_number(q, (int)set_extra[slot][X_PITCH_RANDOM]);
    q = put(q, ",\"a\":"); q = put_number(q, (int)set_extra[slot][X_LEVEL_RANDOM]);
    put(q, ",\"v\":2}");
    if (op1f_value_set(CARD, OP1F_KEEP_SYNTH, slot, KEY, text) == 0) unsaved &= ~(1u << slot);
}

// ---------------------------------------------------------------- the keys (input thread)
static volatile u32 pending_click;                    // bits 0..3 presses, 4..7 presses with shift
static volatile int pending_turn[4];                  // page B's turns, in notches
// The keys that go down while the page is up and that the pack does not take, noted here and said once each in the
// log by the frame function. Nothing is done with them: the line tells which number the unit's keys have (the drop
// key's among them; DROP.md).
static volatile u32 other_keys[4];                    // a bit a key number, 0..127
static volatile u32 other_keys_shift[4];              // the same, for a key that went down with shift held
extern "C" int veffgranular_input(const Op1fInput* e) OP1F_NAMED("veffgranular_input");
extern "C" int veffgranular_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if (e->what == (u32)OP1F_TURN && e->key < 4u && e->shift && shift_page) {      // page B's values are ours to turn
        const int steps = e->turn >> 10;
        if (steps) pending_turn[e->key] += steps;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        pending_click |= (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) ? 16u << knob : 1u << knob;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)PRESS_FIRST && e->key <= (u32)PRESS_LAST) return 1;
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && (e->key < 13u || e->key > 53u)) {   // the drop experiment
        const int t = dropx::key_down(e->key, drop_key, e->what == (u32)OP1F_DOWN_SHIFT || e->shift, 1u, drop_armed, drop_busy);
        if (t == 1) drop_request = (shown_where & 7u) + 1u;       // one word; the frame function does the rest
        if (t) return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key < 128u && (e->key < 13u || e->key > 53u))
        ((e->what == (u32)OP1F_DOWN_SHIFT || e->shift) ? other_keys_shift : other_keys)[e->key >> 5] |= 1u << (e->key & 31u);   // one word written; not taken
    return 0;
}

// ---------------------------------------------------------------- the page
using namespace veffui;
static const char* const MODE_NAMES[gran::MODES] = { "CLASSIC", "LOOP", "CLOUD" };
static const char* const LOOP_NAMES[gran::LOOPS] = { "FREE", "1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS" };

static void icon_grains(void* c) {                    // three grains: little windows over a line
    const int x = 432, y = 10;
    op1f_paint_line(c, x, y + 22, x + 38, y + 22, 1, VEFF_WHITE);
    static const signed char H[3][2] = { { 6, 14 }, { 18, 20 }, { 30, 10 } };
    for (int k = 0; k < 3; ++k) {
        const int cx = x + H[k][0], h = H[k][1];
        op1f_paint_line(c, cx - 5, y + 22, cx, y + 22 - h, 1, VEFF_WHITE);
        op1f_paint_line(c, cx, y + 22 - h, cx + 5, y + 22, 1, VEFF_WHITE);
    }
}

static void paint_art(void* c, u32 where, const GrainSource& s, int values[8], int on) {
    const int L = VEFF_AXIS_L, W = VEFF_AXIS_R - VEFF_AXIS_L, Y = VEFF_AXIS_Y;
    const u32 slot = where & 7u;
    char line[64], * q;
    // the source, top right of the band
    q = line;
    if (sampler_on && sampler_src[slot].valid) { q = put(q, "SAMPLER "); q = put_number(q, (int)slot + 1); }
    else if (file_loaded && set_file[slot] == file_loaded) q = put(q, files.name);
    else { q = put(q, "FILE "); q = put_number(q, (int)set_file[slot]); }
    if (set_mode[slot] == gran::MODE_LOOP) { q = put(q, "  "); q = put(q, LOOP_NAMES[held((int)set_loop[slot], 0, gran::LOOPS - 1)]); }
    if (values[U_REVERSE] > 0) { q = put(q, "  REV"); }
    text(c, VEFF_AXIS_R - 30 - text_width(line, 2, 1), VEFF_ART_TOP - 2, line, VEFF_GREY, 2, 1);
    {   // the grain's shape: where its top stands
        const int gx = VEFF_AXIS_R - 24, gy = VEFF_ART_TOP - 2, top = (SHAPE_TOP[held(values[U_SHAPE], -100, 100) + 100] * 24) >> 8;
        op1f_paint_line(c, gx, gy + 10, gx + top, gy, 1, on ? VEFF_PINK : VEFF_GREY);
        op1f_paint_line(c, gx + top, gy, gx + 24, gy + 10, 1, on ? VEFF_PINK : VEFF_GREY);
    }
    if (!grain_source_ok(s)) {
        axis(c, VEFF_DARK);
        q = line;
        if (files.error == 101) q = put(q, "NO MEMORY FOR A SAMPLE");
        else if (files.error == 102) q = put(q, "FILE OVER 4 MB");
        else if (files.error && files.error < 100u) { q = put(q, "FILE NOT TAKEN: "); q = put(q, sfile::ERROR_TEXT[files.error < 9u ? files.error : 2u]); }
        else if (sampler_on && (scan_pending >> slot & 1u)) q = put(q, "READING THE SAMPLER");
        else if (sampler_on && sampler_seen[slot].bad == (u32)SAMPLER_EMPTY) { q = put(q, "SAMPLER "); q = put_number(q, (int)slot + 1); q = put(q, " EMPTY: LIFT, THEN DROP HERE"); }
        else { q = put(q, "NO SAMPLE: PUT "); q = put_number(q, (int)set_file[slot]); q = put(q, ".WAV IN VEFFGRANULAR"); }
        text(c, VEFF_MID_X - (text_width(line, 2, 2) >> 1), Y - 4, line, on ? VEFF_PINK : VEFF_GREY, 2, 2);
        return;
    }
    // the overview, 216 columns of 2 px
    for (int k = 0; k < COLS; ++k) {
        const int x = L + 2 * k;
        const int hi = (ov_hi[k] * 50) >> 6, lo = (ov_lo[k] * 50) >> 6;
        op1f_paint_line(c, x, Y - hi, x, Y - lo, 1, VEFF_DARK);
    }
    axis(c, VEFF_GREY);
    const u32 len = s.end - s.start;
    // the position, the spray's reach and (loop) the loop's start
    const int px = L + (int)gran::mul_shift((u32)W, (u32)held(values[gran::V_POS], 0, 1000) * 4294967u, 32);
    u32 spray = (u32)held(values[gran::V_SPRAY], 0, 100) * 882u;
    spray = gran::mul_shift(spray, s.rate_q16, 16);
    if (spray > len >> 1) spray = len >> 1;
    const u32 xs = frac32(1u, len);                   // 2^32 / len (len >= 2)
    const int sw = (int)gran::mul_shift(gran::mul_shift(spray, xs, 0), (u32)W, 32);
    if (sw > 0) op1f_paint_box(c, px - sw, VEFF_ART_BOTTOM - 8, 2 * sw + 1, 3, pink(110));
    op1f_paint_line(c, px, VEFF_ART_TOP + 10, px, VEFF_ART_BOTTOM - 4, 1, VEFF_WHITE);
    // the grains every voice plays, as they move: a pink stroke, as tall as its window is open
    for (int v = 0; v < VOICES; ++v) for (int k = 0; k < gran::GRAINS; ++k) {
        const u32 lv = vis[v].level[k];
        if (!lv) continue;
        const u32 pos = vis[v].pos[k];
        if (pos < s.start || pos >= s.end) continue;
        const int x = L + (int)gran::mul_shift(gran::mul_shift(pos - s.start, xs, 0), (u32)W, 32);
        const int h = 4 + (int)((lv * 44u) >> 15);
        const unsigned colour = on ? pink(256) : VEFF_GREY;
        if (vis[v].rev[k]) {                           // a grain read backward: hollow
            op1f_paint_box(c, x - 1, Y - h, 1, 2 * h + 1, colour); op1f_paint_box(c, x + 1, Y - h, 1, 2 * h + 1, colour);
            op1f_paint_box(c, x - 1, Y - h, 3, 1, colour); op1f_paint_box(c, x - 1, Y + h, 3, 1, colour);
        } else op1f_paint_box(c, x - 1, Y - h, 3, 2 * h + 1, colour);
        sparkle(c, x, Y - h - 4, 3, VEFF_WHITE);
    }
}

static void paint(void* c, u32 where, u32 how, int shift) {
    const int on = !(how & (u32)OP1F_SHOWN_OFF);
    op1f_paint_box(c, 0, 0, VEFF_W, VEFF_H, OP1F_COLOUR(0, 0, 0));
    int v[8];
    for (u32 i = 0; i < 8u; ++i) { v[i] = VALUE_START[i]; int x; if (op1f_param_heard(CARD, ASK_ENGINE, where, i, &x) == 0) v[i] = x; }
    const GrainSource* src = source_for(where & 7u);
    GrainSource s;
    s.valid = src->valid; s.data = src->data; s.frames = src->frames; s.stereo = src->stereo; s.start = src->start;
    s.end = src->end; s.root = src->root; s.rate_q16 = src->rate_q16; s.generation = src->generation;
    overview_step(s);
    paint_art(c, where, s, v, on);
    char t[4][16];
    Column col[4];
    if (!shift) {
        put_tenths(t[0], held(v[0], 0, 1000));                     // position in tenths of a percent
        { const u32 ms10 = SIZE_MS10[held(v[1], 0, 100)]; char* q = t[1];
          if (ms10 < 10000u) { q = put_number(q, (int)((ms10 * 6554u) >> 16)); put(q, "MS"); }   // / 10
          else { q = put_tenths(q, (int)((ms10 * 1311u) >> 17)); put(q, "S"); } }               // / 100
        { char* q = put(t[2], "X"); put_tenths(q, (int)OVERLAP10[held(v[2], 0, 100)]); }
        { char* q = put_number(t[3], held(v[3], 0, 100)); put(q, "%"); }
        col[0] = { "POS", t[0], (held(v[0], 0, 1000) * 1049) >> 10 };
        col[1] = { "SIZE", t[1], (held(v[1], 0, 100) * 10486) >> 10 };
        col[2] = { "DENSITY", t[2], (held(v[2], 0, 100) * 10486) >> 10 };
        col[3] = { "SPRAY", t[3], (held(v[3], 0, 100) * 10486) >> 10 };
    } else if (!shift_page) {
        { const int sc = held(v[U_SCAN], -100, 100); char* q = t[0];            // -2.0x .. +2.0x
          *q++ = sc > 0 ? '+' : sc < 0 ? '-' : ' '; q = put_tenths(q, ((sc < 0 ? -sc : sc) * 13107) >> 16); put(q, "X"); }
        { const int sh = held(v[U_SHAPE], -100, 100); char* q = t[1]; if (sh > 0) *q++ = '+'; put_number(q, sh); }
        { const int tu = held(v[U_TUNE], -24, 24); char* q = t[2]; if (tu > 0) *q++ = '+'; q = put_number(q, tu); put(q, "ST"); }
        { char* q = put_number(t[3], held(v[U_REVERSE], 0, 100)); put(q, "%"); }
        col[0] = { "SCAN", t[0], ((held(v[U_SCAN], -100, 100) + 100) * 5243) >> 10 };
        col[1] = { "SHAPE", t[1], ((held(v[U_SHAPE], -100, 100) + 100) * 5243) >> 10 };
        col[2] = { "TUNE", t[2], ((held(v[U_TUNE], -24, 24) + 24) * 21845) >> 10 };
        col[3] = { "REVERSE", t[3], (held(v[U_REVERSE], 0, 100) * 10486) >> 10 };
    } else {
        static const char* const NAMES[EXTRAS] = { "SPREAD", "VOLUME", "P RAND", "L RAND" };
        for (int e = 0; e < EXTRAS; ++e) {
            const int x = held((int)set_extra[where & 7u][e], 0, 100);
            char* q = put_number(t[e], x); put(q, "%");
            col[e] = { NAMES[e], t[e], (x * 10486) >> 10 };
        }
    }
    page(c, "GRANULAR", 99, col, shift ? (shift_page ? "SHIFT B" : "SHIFT A") : MODE_NAMES[held((int)set_mode[where & 7u], 0, gran::MODES - 1)], on);
    icon_grains(c);
    if (how & (u32)OP1F_SHOWN_OFF) text(c, VEFF_MID_X - 40, VEFF_ART_BOTTOM - 20, "SWITCHED OFF", VEFF_PINK, 2, 2);
    if (drop.message) {                               // the drop experiment's word, over the band
        char line[32], * q = line;
        if (drop.message == (u32)dropx::M_DROPPING) q = put(q, "DROPPING");
        else if (drop.message == (u32)dropx::M_DROPPED) q = put(q, own_frames[drop.slot] ? "DROPPED" : "DROP: NOTHING CHANGED");
        else if (drop.message == (u32)dropx::M_NOTHING) q = put(q, "NOTHING LIFTED");
        else { q = put(q, "DROP REFUSED "); q = put_number(q, drop.reason); }
        const int w = text_width(line, 2, 2);
        op1f_paint_box(c, VEFF_MID_X - (w >> 1) - 6, VEFF_ART_TOP + 12, w + 12, 16, OP1F_COLOUR(0, 0, 0));
        text(c, VEFF_MID_X - (w >> 1), VEFF_ART_TOP + 16, line, VEFF_PINK, 2, 2);
    }
}

// ---------------------------------------------------------------- the frame (display thread)
static struct { u32 stood, refusal_said, started, disk_was, round; u32 keys_said[4], keys_shift_said[4], hello, most_said; } fr;

extern "C" void veffgranular_frame(const Op1fFrame* now) OP1F_NAMED("veffgranular_frame");
extern "C" void veffgranular_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    if (!fr.hello) { fr.hello = 1; op1f_log(3, LOG_FILE, 19, "veffgranular: v6 start"); }   // before anything else
    words_this_frame = 0;
    if (!fr.stood) {
        if (op1f_page_stand_in(CARD, ASK_ENGINE, 0)) fr.stood = 1;
        else if (!fr.refusal_said) { fr.refusal_said = 1; op1f_log(2, LOG_FILE, 7, "veffgranular: the base did not take its page"); }
    }
    if (!fr.started) {
        fr.started = 1;
        for (u32 p = 0; p < (u32)SLOTS; ++p) { set_file[p] = 1; for (int e = 0; e < EXTRAS; ++e) set_extra[p][e] = EXTRA_START[e]; }
        op1f_log(3, LOG_FILE, 8, "veffgranular: ready");
    }
    // the kept values of every sound that rides the engine
    const u32 riding = (u32)op1f_riding(CARD, ASK_ENGINE) & 0xffu;
    const u32 todo = reread | (riding & ~ready);
    reread = 0;
    for (u32 p = 0; p < (u32)SLOTS; ++p) if (todo >> p & 1u) kept_read(p);
    const u32 how = shown_how, where = shown_where & 7u;
    // the slot that matters: the one shown, else the one that played last
    const u32 slot = (how & (u32)OP1F_SHOWN_UP) ? where : (playing_slot ? playing_slot - 1u : where);
    // the disk mode: nothing is read while it is open; once it closes the folder is read again
    const u32 disk = now->disk;
    if (disk) { fr.disk_was = 1; }
    else {
        if (fr.disk_was || !files.listed) {
            fr.disk_was = 0; files.listed = 1; files.tried = 0;
            if (op1f_memory(CARD, ASK_MEMORY)) list_folder(); else files.listed = 0;
        }
        const u32 want = set_file[slot];
        if (want >= 1u && want <= (u32)FILES && want != files.tried && (riding >> slot & 1u) && op1f_memory(CARD, ASK_MEMORY)) file_load(want);
        if (sampler_on) {                             // the active slot every frame, one other in turn
            sampler_refresh(slot);
            fr.round = (fr.round + 1u) & 7u;
            if (fr.round != slot) sampler_refresh(fr.round);
            sampler_scan_frame();
            if (words_this_frame > words_most) words_most = words_this_frame;
            if (!scan_pending && scans_done && fr.most_said != words_most && (now->frame & 255u) == 0u) {   // now and then, when it grew
                fr.most_said = words_most;
                op1f_log(3, LOG_FILE, 20, "veffgranular: %u scan(s), at most %u words a frame", scans_done, words_most);
            }
        }
    }
    drop_frame(now, how, where, riding);
    // presses
    if (how & (u32)OP1F_SHOWN_UP) {
        for (int k = 0; k < 4; ++k) {                 // page B's turns: two of a hundred a notch
            const int t = pending_turn[k];
            if (!t) continue;
            pending_turn[k] -= t;
            set_extra[where][k] = held((int)set_extra[where][k] + 2 * t, 0, 100);
            unsaved |= 1u << where;
        }
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 1u) { set_mode[where] = set_mode[where] + 1u >= (u32)gran::MODES ? 0u : set_mode[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 2u) op1f_param_set(CARD, ASK_ENGINE, where, U_SHAPE, 0);
        if (clicks & 4u) { set_loop[where] = set_loop[where] + 1u >= (u32)gran::LOOPS ? 0u : set_loop[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 8u) { set_file[where] = set_file[where] >= (u32)FILES ? 1u : set_file[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 16u) shift_page = !shift_page;   // shift and the 1st knob's press: the other shift page
        if (!shift_page) {                            // with shift, a press puts page A's value back
            static const u32 WHICH[3] = { U_SHAPE, U_TUNE, U_REVERSE };
            for (u32 k = 0; k < 3u; ++k) if (clicks >> (5u + k) & 1u) op1f_param_set(CARD, ASK_ENGINE, where, WHICH[k], VALUE_START[WHICH[k]]);
        } else {                                      // or page B's
            static const u32 WHICH[3] = { X_VOLUME, X_PITCH_RANDOM, X_LEVEL_RANDOM };
            for (u32 k = 0; k < 3u; ++k) if (clicks >> (5u + k) & 1u) { set_extra[where][WHICH[k]] = EXTRA_START[WHICH[k]]; unsaved |= 1u << where; }
        }
    } else { pending_click = 0; for (int k = 0; k < 4; ++k) pending_turn[k] = 0; }
    for (u32 p = 0; p < (u32)SLOTS; ++p) if (unsaved >> p & 1u) kept_write(p);
    for (u32 w = 0; w < 4u; ++w) {                    // a key the page saw and did not take: one line, once a key a start
        const u32 fresh = other_keys[w] & ~fr.keys_said[w];
        if (!fresh) continue;
        u32 bit = 0; while (!(fresh >> bit & 1u)) ++bit;
        fr.keys_said[w] |= 1u << bit;
        op1f_log(3, LOG_FILE, 10, "veffgranular: key %u down on the page, not taken", w * 32u + bit);
    }
    for (u32 w = 0; w < 4u; ++w) {                    // and with shift held: shift and drop is left to the unit on purpose
        const u32 fresh = other_keys_shift[w] & ~fr.keys_shift_said[w];
        if (!fresh) continue;
        u32 bit = 0; while (!(fresh >> bit & 1u)) ++bit;
        fr.keys_shift_said[w] |= 1u << bit;
        op1f_log(3, LOG_FILE, 17, "veffgranular: key %u down with shift on the page, not taken", w * 32u + bit);
    }
    if (now->page == CARD && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP)) paint(now->canvas, where, how, now->shift != 0);
}
VEFF_FAST
