// veffgranular: a granular sample synth engine for the OP-1 Field (open-field level 9/10, OS 1.7.3+0).
//
//   `veffgranular` in the unit's engine list (shift and T1 on a synth sound). Each key plays grains of one sample,
//   pitched from the note against the sample's root. Three modes: classic (grains at the position), loop (grains
//   follow a head looping a short region, free or a tempo length) and cloud (randomised dense grains).
//   Knobs: position, size, density, spray; with shift: scan, spread, tune, volume (the eight engine values, kept with
//   the sound by the unit). Mode, window, loop length and file number are kept with the sound through level 7
//   (`veffgranular_set`).
//   Presses: 1st mode, 2nd window, 3rd loop length, 4th the next file; with shift a press resets scan, spread, tune,
//   volume.
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
#include "veff_ui.h"
#include "granular_core.h"
#include "sample_file.h"
#include "sampler_region.h"

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
static const int VALUE_START[8] = { 250, 50, 50, 10, 0, 30, 0, 80 };   // as the recipe's starts

// ---------------------------------------------------------------- shared between the threads (single words)
static volatile u32 set_mode[SLOTS], set_window[SLOTS], set_loop[SLOTS], set_file[SLOTS];   // kept values
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
static const GrainSource* source_for(u32 slot) {
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
    gran::Settings set;
    set.mode = set_mode[slot]; set.window = set_window[slot]; set.loop = set_loop[slot];
    gran::Block b;
    b.values = now->values; b.step = now->step; b.period = now->period; b.voice = now->voice; b.out = out;
    b.events = ((now->events & (u32)OP1F_VOICE_BEGINS) ? (u32)gran::EV_BEGINS : 0u) |
               ((now->events & (u32)OP1F_VOICE_ENDS) ? (u32)gran::EV_ENDS : 0u) |
               ((now->events & (u32)OP1F_VOICE_FRESH) ? (u32)gran::EV_FRESH : 0u);
    gran::render(voices[now->voice], s, set, b, &vis[now->voice]);
}

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
    if (on != sampler_on) op1f_log(3, LOG_FILE, 1, "veffgranular: the sampler source is %s (sampler.off %s)", on ? "on" : "off", on ? "absent" : "found");
    sampler_on = on;
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
    op1f_log(3, LOG_FILE, 4, "veffgranular: %s: %u frames, %s, %u Hz, root %u", files.name, in.frames, in.stereo ? "stereo" : "mono", in.rate, in.root);
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
static SamplerSeen sampler_seen[SLOTS];

static u32 pool_sum(u32 data, u32 bytes) {           // 8 runs of 8 words spread over the data
    u32 sum = 0x9e3779b9u;
    const u32 step = (bytes >> 3) & ~3u;
    for (u32 k = 0; k < 8u; ++k) for (u32 i = 0; i < 8u; ++i) {
        const u32 a = data + k * step + i * 4u;
        if (a + 4u > data + bytes) break;
        sum = (sum ^ rd32(a)) * 0x01000193u;
    }
    return sum;
}

static void sampler_refresh(u32 slot) {
    SamplerSeen s = { 0, 0, 0, 0, 0, 0, 0, 0 };
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
        else s.bad = (u32)sampler_region(s.frames, s.words, s.stereo, &s.first, &s.end);   // 0, 4 (stereo) or 6 (empty)
    }
    if (!s.bad) s.sum = pool_sum(s.data, s.end << (s.stereo ? 2 : 1));
    SamplerSeen& was = sampler_seen[slot];
    if (s.bad) {
        if (was.bad != s.bad) {
            if (s.bad == (u32)SAMPLER_EMPTY) op1f_log(3, LOG_FILE, 5, "veffgranular: the sampler of synth %u holds no sample (words %u)", slot + 1, s.words);
            else op1f_log(2, LOG_FILE, 5, "veffgranular: sampler source of synth %u not valid (check %u)", slot + 1, s.bad);
        }
        sampler_src[slot].valid = 0;
        was = s;
        return;
    }
    if (sampler_src[slot].valid && was.data == s.data && was.frames == s.frames && was.words == s.words && was.stereo == s.stereo && was.sum == s.sum) return;
    // changed (or first seen): the sample is what the last load, drop or recording wrote
    source_publish(&sampler_src[slot], (const short*)s.data, s.end, s.stereo, s.first, s.end, 60, 65536);
    if (was.bad || was.data != s.data || was.words != s.words || was.stereo != s.stereo)
        op1f_log(3, LOG_FILE, 6, "veffgranular: sampler source of synth %u: frames %u to %u of %u, %s", slot + 1, s.first, s.end, s.frames, s.stereo ? "stereo" : "mono");
    was = s;
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
    char text[96];
    set_mode[slot] = 0; set_window[slot] = 0; set_loop[slot] = 0; set_file[slot] = 1;
    if (op1f_value_get(CARD, OP1F_KEEP_SYNTH, slot, KEY, text, sizeof text) > 0) {
        int v;
        if (read_member(text, 'm', &v)) set_mode[slot] = (u32)held(v, 0, gran::MODES - 1);
        if (read_member(text, 'w', &v)) set_window[slot] = (u32)held(v, 0, WINDOW_SHAPES - 1);
        if (read_member(text, 'l', &v)) set_loop[slot] = (u32)held(v, 0, gran::LOOPS - 1);
        if (read_member(text, 'f', &v)) set_file[slot] = (u32)held(v, 1, FILES);
    }
    ready |= 1u << slot;
}
static void kept_write(u32 slot) {                    // {"m":0,"w":0,"l":0,"f":1,"v":1}
    char text[64], * q = text;
    q = put(q, "{\"m\":"); q = put_number(q, (int)set_mode[slot]);
    q = put(q, ",\"w\":"); q = put_number(q, (int)set_window[slot]);
    q = put(q, ",\"l\":"); q = put_number(q, (int)set_loop[slot]);
    q = put(q, ",\"f\":"); q = put_number(q, (int)set_file[slot]);
    put(q, ",\"v\":1}");
    if (op1f_value_set(CARD, OP1F_KEEP_SYNTH, slot, KEY, text) == 0) unsaved &= ~(1u << slot);
}

// ---------------------------------------------------------------- the keys (input thread)
static volatile u32 pending_click;                    // bits 0..3 presses, 4..7 presses with shift
extern "C" int veffgranular_input(const Op1fInput* e) OP1F_NAMED("veffgranular_input");
extern "C" int veffgranular_input(const Op1fInput* e) {
    if (e->bytes < (u32)INPUT_LEVEL_2 || !e->mine) return 0;
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)CLICK_FIRST && e->key <= (u32)CLICK_LAST) {
        const u32 knob = e->key - (u32)CLICK_FIRST;
        pending_click |= (e->what == (u32)OP1F_DOWN_SHIFT || e->shift) ? 16u << knob : 1u << knob;
        return 1;
    }
    if ((e->what == (u32)OP1F_DOWN || e->what == (u32)OP1F_DOWN_SHIFT) && e->key >= (u32)PRESS_FIRST && e->key <= (u32)PRESS_LAST) return 1;
    return 0;
}

// ---------------------------------------------------------------- the page
using namespace veffui;
static const char* const MODE_NAMES[gran::MODES] = { "CLASSIC", "LOOP", "CLOUD" };
static const char* const WINDOW_NAMES[WINDOW_SHAPES] = { "SOFT", "FLAT", "PLUCK", "TRI" };
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
    q = put(q, "  "); q = put(q, WINDOW_NAMES[set_window[slot] & 3u]);
    if (set_mode[slot] == gran::MODE_LOOP) { q = put(q, "  "); q = put(q, LOOP_NAMES[held((int)set_loop[slot], 0, gran::LOOPS - 1)]); }
    text(c, VEFF_AXIS_R - text_width(line, 2, 1), VEFF_ART_TOP - 2, line, VEFF_GREY, 2, 1);
    if (!grain_source_ok(s)) {
        axis(c, VEFF_DARK);
        q = line;
        if (files.error == 101) q = put(q, "NO MEMORY FOR A SAMPLE");
        else if (files.error == 102) q = put(q, "FILE OVER 4 MB");
        else if (files.error && files.error < 100u) { q = put(q, "FILE NOT TAKEN: "); q = put(q, sfile::ERROR_TEXT[files.error < 9u ? files.error : 2u]); }
        else if (sampler_on && sampler_seen[slot].bad == (u32)SAMPLER_EMPTY) { q = put(q, "SAMPLER "); q = put_number(q, (int)slot + 1); q = put(q, " EMPTY: DROP IN THE SAMPLER"); }
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
        op1f_paint_box(c, x - 1, Y - h, 3, 2 * h + 1, on ? pink(256) : VEFF_GREY);
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
    } else {
        { const int sc = held(v[4], -100, 100); char* q = t[0];                 // -2.0x .. +2.0x
          *q++ = sc > 0 ? '+' : sc < 0 ? '-' : ' '; q = put_tenths(q, ((sc < 0 ? -sc : sc) * 13107) >> 16); put(q, "X"); }
        { char* q = put_number(t[1], held(v[5], 0, 100)); put(q, "%"); }
        { const int tu = held(v[6], -24, 24); char* q = t[2]; if (tu > 0) *q++ = '+'; q = put_number(q, tu); put(q, "ST"); }
        { char* q = put_number(t[3], held(v[7], 0, 100)); put(q, "%"); }
        col[0] = { "SCAN", t[0], ((held(v[4], -100, 100) + 100) * 5243) >> 10 };
        col[1] = { "SPREAD", t[1], (held(v[5], 0, 100) * 10486) >> 10 };
        col[2] = { "TUNE", t[2], ((held(v[6], -24, 24) + 24) * 21845) >> 10 };
        col[3] = { "VOLUME", t[3], (held(v[7], 0, 100) * 10486) >> 10 };
    }
    page(c, "GRANULAR", 99, col, shift ? "SHIFT" : MODE_NAMES[held((int)set_mode[where & 7u], 0, gran::MODES - 1)], on);
    icon_grains(c);
    if (how & (u32)OP1F_SHOWN_OFF) text(c, VEFF_MID_X - 40, VEFF_ART_BOTTOM - 20, "SWITCHED OFF", VEFF_PINK, 2, 2);
}

// ---------------------------------------------------------------- the frame (display thread)
static struct { u32 stood, refusal_said, started, disk_was, round; } fr;

extern "C" void veffgranular_frame(const Op1fFrame* now) OP1F_NAMED("veffgranular_frame");
extern "C" void veffgranular_frame(const Op1fFrame* now) {
    if (now->bytes < (u32)FRAME_LEVEL_4) return;
    if (!fr.stood) {
        if (op1f_page_stand_in(CARD, ASK_ENGINE, 0)) fr.stood = 1;
        else if (!fr.refusal_said) { fr.refusal_said = 1; op1f_log(2, LOG_FILE, 7, "veffgranular: the base did not take its page"); }
    }
    if (!fr.started) {
        fr.started = 1;
        for (u32 p = 0; p < (u32)SLOTS; ++p) { set_file[p] = 1; }
        op1f_log(3, LOG_FILE, 8, "veffgranular: ready; the synth sampler's sample of each slot, else files 1..8 in the folder veffgranular; sampler.off there switches the sampler source off");
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
        }
    }
    // presses
    if (how & (u32)OP1F_SHOWN_UP) {
        const u32 clicks = pending_click;
        pending_click &= ~clicks;
        if (clicks & 1u) { set_mode[where] = set_mode[where] + 1u >= (u32)gran::MODES ? 0u : set_mode[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 2u) { set_window[where] = set_window[where] + 1u >= (u32)WINDOW_SHAPES ? 0u : set_window[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 4u) { set_loop[where] = set_loop[where] + 1u >= (u32)gran::LOOPS ? 0u : set_loop[where] + 1u; unsaved |= 1u << where; }
        if (clicks & 8u) { set_file[where] = set_file[where] >= (u32)FILES ? 1u : set_file[where] + 1u; unsaved |= 1u << where; }
        for (u32 k = 0; k < 4u; ++k) if (clicks >> (4u + k) & 1u) op1f_param_set(CARD, ASK_ENGINE, where, 4u + k, VALUE_START[4u + k]);
    } else pending_click = 0;
    for (u32 p = 0; p < (u32)SLOTS; ++p) if (unsaved >> p & 1u) kept_write(p);
    if (now->page == CARD && now->canvas != 0 && (how & (u32)OP1F_SHOWN_UP)) paint(now->canvas, where, how, now->shift != 0);
}
