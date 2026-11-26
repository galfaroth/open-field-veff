// veffchop on the computer: veffchop.cpp itself, with the unit's memory and the base's calls stood in. Run it under the
// address and undefined-behaviour sanitizers (tests/run_tests.sh). It checks:
//   - the Q31 conversion is exact for every frame 0..882000 (against 64-bit arithmetic), and udiv against '/';
//   - the first frame logs 'veffchop: start' and reads no sample word; no frame reads more than BUDGET sample words;
//   - transient: on synthetic drum loops with known hits every hit is found at the most sensitivity, each cut at most
//     10 ms before its hit and never after it; fewer cuts at a lower sensitivity, the strongest kept;
//   - lazy: every cut on a zero crossing of L+R, no two cuts closer than GAP;
//   - equal: N parts, equal to a frame;
//   - mapping: ORDER, WRAP, KIT (the kicks on keys 1-2, the hats on 9/11, the snares on 3-4);
//   - apply: only +4/+8 of the current slot's 24 records change, no write leaves a start past its end, the pairs are
//     the slices' Q31; undo puts back every word; a press while the sample changes writes nothing;
//   - mono and an unknown stereo byte are refused; a changed sample is analysed again.
// It writes the page (960x448: drawn at 4x, kept at 2x) as PPM files into the folder given as the first argument.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <cstdarg>

#define VEFFCHOP_HOST 1
#include "../veffchop.cpp"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

// ---------------------------------------------------------------- the unit's memory, stood in
static const u32 HOLDER_AT = 0x85000000u, OBJ_AT = 0x85100000u, DATA_AT = 0x86000000u;
static std::map<u32, u32> words_mem;                     // single words: CUR_SLOT, holders, object fields, key table
static std::vector<uint32_t> sample;                     // the stereo frames, at DATA_AT + 16
static u32 stereo_byte = 1;
static long wild = 0, writes_seen = 0, bad_order = 0, wrong_place = 0;
static std::map<u32, u32> pool_mem;                      // v8: words pasted behind the sample
static long pool_writes = 0, pool_wild = 0;
void host_pool_wr32(u32 a, u32 v) {
    ++pool_writes;
    const u32 material = (u32)sample.size() * (stereo_byte ? 4u : 2u);   // a mono sample's frames are halfwords
    if ((a & 3u) || a < DATA_AT + 16 + material || a >= DATA_AT + 0x35d540u) { ++pool_wild; return; }
    pool_mem[a] = v;
}
u32 host_rd32(u32 a) {
    { auto q = pool_mem.find(a); if (q != pool_mem.end()) return q->second; }
    if (a >= DATA_AT + 16 && a < DATA_AT + 16 + sample.size() * 4) return sample[(a - DATA_AT - 16) >> 2];
    if (a >= DATA_AT && a < DATA_AT + 0x35d540u) { auto p = pool_mem.find(a); return p == pool_mem.end() ? 0u : p->second; }
    auto it = words_mem.find(a);
    if (it == words_mem.end()) { ++wild; return 0; }
    return it->second;
}
u32 host_rd8(u32 a) { if (a == OBJ_AT + OFF_STEREO) return stereo_byte; ++wild; return 0; }
static u32 stereo_byte_of() { return stereo_byte; }
static u32 cur_slot() { return words_mem[CUR_SLOT]; }
static u32 rec(u32 slot, u32 k, u32 off) { return words_mem[KEY_TABLE + slot * KIT_BYTES + k * KEY_BYTES + off]; }
void host_wr32(u32 a, u32 v) {
    ++writes_seen;
    const u32 t = KEY_TABLE + cur_slot() * KIT_BYTES;
    if (a < t || a >= t + KIT_BYTES || ((a - t) & 0x1f) != 4u && ((a - t) & 0x1f) != 8u) { ++wrong_place; return; }
    words_mem[a] = v;
    const u32 k = (a - t) >> 5;
    if (rec(cur_slot(), k, 4) > rec(cur_slot(), k, 8)) ++bad_order;
}

static std::vector<std::string> log_lines;
extern "C" void op1f_log(int, const char*, int, const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    log_lines.push_back(b);
}
extern "C" const Op1fCard op1f_card_veffchop = {};
extern "C" int op1f_page_stand_in(const Op1fCard*, unsigned, void*) { return 1; }
// v6: the base's sample helpers, stood in. get: the stood-in object's sample (its channels from the stereo byte, 0 or 1
// as the base hands it), `dropped` and `done` as the test sets them. drop: the answer the test sets, the place noted.
static u32 mock_dropped = 0, mock_done = 0, drop_calls = 0, drop_place_seen = 0;
static int mock_drop_answer = 7, mock_answer = 0;
static u32 stereo_byte_of();
extern "C" int op1f_sample_get(const Op1fCard*, unsigned place, Op1fSample* out) {
    if (place < 8u || place > 15u) return OP1F_BAD_NAME;
    out->bytes = sizeof(Op1fSample); out->data = (const short*)(uintptr_t)0x86000000u; out->stereo = stereo_byte_of() ? 1u : 0u;
    out->room = 0; out->capacity = 882000; out->dropped = mock_dropped; out->ticket = 0; out->done = mock_done; out->answer = mock_answer;
    return 0;
}
extern "C" int op1f_sample_drop(const Op1fCard*, unsigned, unsigned place) { ++drop_calls; drop_place_seen = place; return mock_drop_answer; }
// v8: the memory ask (1, the copy) and the drum mark
static std::vector<short> clip_memory((1764000 + 57344) / 2, 0);   // v9: the copy's room, then the analysis
static bool memory_given = true;
extern "C" void* op1f_memory(const Op1fCard*, unsigned ask) { return ask == 1 && memory_given ? clip_memory.data() : nullptr; }
static u32 changed_calls = 0, changed_place = 0;
extern "C" int op1f_sample_changed(const Op1fCard*, unsigned ask, unsigned place) { ++changed_calls; changed_place = place; return ask == 0 ? 9 : OP1F_BAD_NAME; }

// the canvas: drawn at 4x, kept at 2x
enum { SS = 4, CW = 480 * SS, CH = 224 * SS };
static std::vector<uint8_t> canvas_rgb(CW * CH * 3);
static long paints = 0;
static void plot(int x, int y, unsigned c) {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return;
    uint8_t* p = &canvas_rgb[(size_t)(y * CW + x) * 3];
    p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); p[2] = (uint8_t)((c & 31) * 255 / 31);
}
extern "C" void op1f_paint_box(void*, int x, int y, int w, int h, unsigned c) {
    ++paints;
    for (int j = y * SS; j < (y + h) * SS; ++j) for (int i = x * SS; i < (x + w) * SS; ++i) plot(i, j, c);
}
extern "C" void op1f_paint_line(void*, int x0, int y0, int x1, int y1, int width, unsigned c) {
    ++paints;
    const double ax = (x0 + 0.5) * SS, ay = (y0 + 0.5) * SS, bx = (x1 + 0.5) * SS, by = (y1 + 0.5) * SS;
    const double r = width * SS * 0.5;
    const int lx = (int)std::floor(std::fmin(ax, bx) - r), hx = (int)std::ceil(std::fmax(ax, bx) + r);
    const int ly = (int)std::floor(std::fmin(ay, by) - r), hy = (int)std::ceil(std::fmax(ay, by) + r);
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int y = ly; y <= hy; ++y) for (int x = lx; x <= hx; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const double qx = ax + t * dx - px, qy = ay + t * dy - py;
        if (qx * qx + qy * qy <= r * r) plot(x, y, c);
    }
}
static void save_ppm(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", CW / 2, CH / 2);
    for (int y = 0; y < CH; y += 2) for (int x = 0; x < CW; x += 2) for (int c = 0; c < 3; ++c) {
        const int sum = canvas_rgb[((size_t)y * CW + x) * 3 + c] + canvas_rgb[((size_t)y * CW + x + 1) * 3 + c]
                      + canvas_rgb[((size_t)(y + 1) * CW + x) * 3 + c] + canvas_rgb[((size_t)(y + 1) * CW + x + 1) * 3 + c];
        std::fputc(sum / 4, f);
    }
    std::fclose(f);
}

// ---------------------------------------------------------------- the kit and the sample
static uint32_t rng = 12345;
static double noise() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) / 8388608.0) - 1.0; }
enum Kind { KICK, SNARE, HAT };
struct Hit { u32 at; Kind kind; double gain; };
static std::vector<Hit> hits;

static void make_loop(u32 frames, const std::vector<Hit>& hs) {
    std::vector<double> L(frames, 0.0), R(frames, 0.0);
    for (u32 i = 0; i < frames; ++i) { L[i] = 0.002 * noise(); R[i] = 0.002 * noise(); }   // a little hiss
    for (const Hit& h : hs) {
        const u32 len = h.kind == KICK ? 13000 : h.kind == SNARE ? 8000 : 2200;
        double lp = 0, ph = 0;
        for (u32 i = 0; i < len && h.at + i < frames; ++i) {
            const double t = i / 44100.0;
            double v;
            if (h.kind == KICK) { ph += 2 * M_PI * (50 + 90 * std::exp(-t * 30)) / 44100; v = std::sin(ph) * std::exp(-t * 9); }
            else if (h.kind == SNARE) { lp += 0.5 * (noise() - lp); v = (0.6 * lp + 0.4 * std::sin(2 * M_PI * 190 * t)) * std::exp(-t * 22); }
            else { const double n = noise(); v = (n - lp) * std::exp(-t * 90) * 0.5; lp = n; }
            L[h.at + i] += h.gain * v; R[h.at + i] += h.gain * v * 0.9;
        }
    }
    sample.assign(frames, 0);
    for (u32 i = 0; i < frames; ++i) {
        const int l = (int)std::lround(std::fmax(-1, std::fmin(1, L[i])) * 30000), r = (int)std::lround(std::fmax(-1, std::fmin(1, R[i])) * 30000);
        sample[i] = (uint32_t)(uint16_t)(int16_t)l | ((uint32_t)(uint16_t)(int16_t)r << 16);
    }
}

// A basic beat at 120 bpm: kick on 1 and 3, snare on 2 and 4, hats on the eighths, `bars` bars.
static std::vector<Hit> beat(u32 bars, u32 first) {
    std::vector<Hit> hs;
    const u32 eighth = 11025;
    for (u32 b = 0; b < bars; ++b) for (u32 e = 0; e < 8; ++e) {
        const u32 at = first + (b * 8 + e) * eighth;
        if (e == 0 || e == 4) hs.push_back({ at, KICK, 0.9 });
        else if (e == 2 || e == 6) hs.push_back({ at, SNARE, 0.8 });
        else hs.push_back({ at, HAT, 0.35 });
    }
    return hs;
}

static void setup_unit(u32 slot, u32 frames, u32 stereo) {
    words_mem.clear(); pool_mem.clear();
    words_mem[CUR_SLOT] = slot;
    for (u32 s = 0; s < 8; ++s) words_mem[HOLDERS + s * 4] = s == slot ? HOLDER_AT : 0;
    words_mem[HOLDER_AT] = OBJ_AT;
    words_mem[OBJ_AT] = DATA_AT;
    words_mem[OBJ_AT + OFF_WORDS] = stereo ? 2 * frames + 8 : frames + 8;
    stereo_byte = stereo;
    for (u32 s = 0; s < 8; ++s) for (u32 k = 0; k < 24; ++k) {           // a stock-looking kit: 24 equal pieces
        const u32 a = KEY_TABLE + s * KIT_BYTES + k * KEY_BYTES;
        const u32 st0 = q31_of_frame(k * (frames / 24)), en0 = q31_of_frame((k + 1) * (frames / 24)) - 0x962d;
        words_mem[a + 0] = 0x18000000u + k; words_mem[a + 4] = st0; words_mem[a + 8] = en0; words_mem[a + 12] = 0x30000000u;
        words_mem[a + 16] = 0x20000000u; words_mem[a + 20] = 0x40000000u; words_mem[a + 24] = 0; words_mem[a + 28] = 0x20000000u;
    }
}

// ---------------------------------------------------------------- driving the pack
static unsigned ms = 1000, frame_no = 0;
static u32 max_words = 0;
static void frame(int shift = 0, bool page = true) {
    Op1fFrame fr = {};
    fr.bytes = sizeof fr; fr.frame = ++frame_no; fr.ms = (ms += 17);
    fr.page = page ? &op1f_card_veffchop : 0; fr.canvas = page ? &fr : 0; fr.shift = (unsigned)shift;
    veffchop_frame(&fr);
    if (words_read > max_words) max_words = words_read;
}
static void turn(int knob, int steps, int shift = 0) {
    Op1fInput e = {}; e.bytes = sizeof e; e.mine = 1; e.what = OP1F_TURN; e.key = (unsigned)knob; e.turn = steps * 1024; e.shift = (unsigned)shift;
    veffchop_input(&e);
}
static void press(int knob, int shift = 0) {
    Op1fInput e = {}; e.bytes = sizeof e; e.mine = 1; e.what = shift ? OP1F_DOWN_SHIFT : OP1F_DOWN; e.key = 54u + (unsigned)knob; e.shift = (unsigned)shift;
    veffchop_input(&e);
    e.what = OP1F_UP; veffchop_input(&e);
}
// v2: a keyboard key reaches the card's input function and is let through; v3: by the unit's numbers
static void strike(int k) {
    unsigned rec[4] = { KEY_NUMBER[k], 100u, 0u, 0u };
    Op1fInput e = {}; e.bytes = sizeof e; e.mine = 1; e.what = OP1F_DOWN; e.key = KEY_NUMBER[k]; e.record = rec;
    CHECK(veffchop_input(&e) == 0, "keyboard key %d was taken", k);
    e.what = OP1F_UP; e.record = 0;
    CHECK(veffchop_input(&e) == 0, "keyboard key %d's up was taken", k);
}
static void set(int knob, int value_now, int want, int shift = 0) { turn(knob, want - value_now, shift); }
static int finishing_frames = 0;
static int until_ready(int most = 2000) {
    int n = 1;
    frame();
    while (st.state != READY && n < most) { const int f = st.state == FINISHING; frame(); finishing_frames += f; ++n; }
    frame();
    return n;
}

static std::string out_dir;
static void picture(const char* name) { if (!out_dir.empty()) save_ppm(out_dir + "/" + name); }

// the hit each cut belongs to, or -1
static int cut_ok(u32 cut, const std::vector<Hit>& hs) {
    for (size_t i = 0; i < hs.size(); ++i) if (cut <= hs[i].at && hs[i].at - cut <= 2u * WIN) return (int)i;
    return -1;
}

int main(int argc, char** argv) {
    out_dir = argc > 1 ? argv[1] : "";

    // ---- arithmetic
    {
        u32 bad = 0;
        for (u32 f = 0; f <= 882000u; ++f) {
            const uint64_t want = f >= 882000u ? 0x7fffffffu : ((uint64_t)f << 31) / 882000u;
            if (q31_of_frame(f) != want) ++bad;
            const u32 back = frame_of_q31((u32)want);
            const uint64_t exact = (want * 882000u) >> 31;
            if (back + 1 < exact || back > exact + 1) ++bad;
        }
        CHECK(bad == 0, "q31_of_frame / frame_of_q31: %u wrong", bad);
        u32 bad_mono = 0;                                // v6: the mono capacity, 529200
        for (u32 f = 0; f <= 529200u; ++f) {
            const uint64_t want = f >= 529200u ? 0x7fffffffu : ((uint64_t)f << 31) / 529200u;
            if (q31_of_frame(f, 529200u) != want) ++bad_mono;
            const u32 back = frame_of_q31((u32)want, 529200u);
            const uint64_t exact = (want * 529200u) >> 31;
            if (back + 1 < exact || back > exact + 1) ++bad_mono;
        }
        CHECK(bad_mono == 0, "mono q31_of_frame / frame_of_q31: %u wrong", bad_mono);
        CHECK(capacity_of(1) == 882000u && capacity_of(0) == 529200u, "capacities");
        std::printf("q31 mono: every frame 0..529200 exact (e.g. 516838 -> %08x)\n", q31_of_frame(516838, 529200u));
        std::printf("q31: every frame 0..882000 exact (e.g. 810082 -> %08x, 1 -> %08x)\n", q31_of_frame(810082), q31_of_frame(1));
        u32 bd = 0;
        for (int i = 0; i < 200000; ++i) { u32 n = (u32)(noise() * 2e9 + 2e9), d = (u32)((noise() + 1.0) * 1e6) + 1; if (udiv(n, d) != n / d) ++bd; }
        CHECK(bd == 0, "udiv: %u wrong", bd);
    }

    // ---- the first frame: the start line, nothing read
    hits = beat(4, 2000);                                // 4 bars at 120 bpm: 8 s
    const u32 FR = 4 * 8 * 11025 + 6000;
    make_loop(FR, hits);
    setup_unit(1, FR, 1);
    frame();
    CHECK(!log_lines.empty() && log_lines[0] == "veffchop: start", "the first line is '%s'", log_lines.empty() ? "" : log_lines[0].c_str());
    CHECK(words_read == 0 && wild == 0, "the first frame read %u sample words, %ld wild", words_read, wild);
    // v2: the drum's page: off until shown(OP1F_PLACE_DRUM + slot, OP1F_SHOWN_UP); a turn is not taken while it is off
    {
        Op1fInput e = {}; e.bytes = sizeof e; e.mine = 1; e.what = OP1F_TURN; e.key = 0; e.turn = 1024;
        CHECK(veffchop_input(&e) == 0, "a turn taken before the page is up");
    }
    veffchop_shown(OP1F_PLACE_DRUM + 1, OP1F_SHOWN_UP);
    CHECK(page_up(), "the page is not up at where %u", (u32)OP1F_PLACE_DRUM + 1);
    veffchop_shown(1, OP1F_SHOWN_UP);
    CHECK(!page_up(), "a synth place taken for the drum's");
    veffchop_shown(OP1F_PLACE_DRUM + 1, 0);
    CHECK(!page_up(), "the page up after shown(.., 0)");
    veffchop_shown(OP1F_PLACE_DRUM, OP1F_SHOWN_UP);
    {
        Op1fInput e = {}; e.bytes = sizeof e; e.mine = 1; e.what = OP1F_DOWN; e.key = 38;
        CHECK(veffchop_input(&e) == 0, "shift taken");
        e.key = 53; CHECK(veffchop_input(&e) == 0, "key 53 taken");
        e.what = OP1F_TURN; e.key = 3; e.turn = 0; CHECK(veffchop_input(&e) == 1, "a turn of the fourth knob not taken");
        CHECK(drum_key_of(39) == 0 && drum_key_of(13) == 1 && drum_key_of(32) == 8 && drum_key_of(31) == 12 && drum_key_of(53) == 23 &&
              drum_key_of(38) == -1 && drum_key_of(21) == -1 && drum_key_of(54) == -1, "drum_key_of");
        int seen[24] = {}, all = 1;
        for (unsigned n = 0; n < 128; ++n) { const int k = drum_key_of(n); if (k >= 0) ++seen[k]; }
        for (int k = 0; k < 24; ++k) all &= seen[k] == 1;
        CHECK(all, "the 24 drum keys are not each one number");
        // v3: shift seen by its own down and up (the press's flag 0): a turn held goes to the shift set
        Op1fInput sh = {}; sh.bytes = sizeof sh; sh.mine = 1; sh.what = OP1F_DOWN; sh.key = 38;
        veffchop_input(&sh);
        const int gap_before = turned_shift[1];
        e.what = OP1F_TURN; e.key = 1; e.turn = 1024; e.shift = 0; veffchop_input(&e);
        CHECK(turned_shift[1] == gap_before + 1024, "a turn with shift held (flag 0) not on the shift set");
        e.turn = -1024; veffchop_input(&e);
        sh.what = OP1F_UP; veffchop_input(&sh);
        CHECK(!shift_held, "shift still held after its up");

    }

    // ---- waiting, then analysing within the budget
    int waited = 0;
    while (st.state != ANALYSING && waited < 100) { frame(); ++waited; }
    std::printf("analysis began after %d frames (%u ms)\n", waited, waited * 17);
    CHECK(waited * 17 >= STEADY_MS && waited < 60, "steadying took %d frames", waited);
    for (int i = 0; i < 20; ++i) frame();
    picture("page-analysing.ppm");
    const int took = until_ready();
    // v2: the candidates built over frames, the same as in one go
    {
        static Analysis again;
        std::memcpy(&again, &an, sizeof an);
        finish(again);
        bool same = again.ncand == an.ncand && again.max_rise == an.max_rise;
        for (u32 i = 0; same && i < an.ncand; ++i) same = again.cpos[i] == an.cpos[i] && again.cstr[i] == an.cstr[i];
        CHECK(same, "finish_step over frames gave %u candidates, finish() %u", an.ncand, again.ncand);
        const u32 nw = an.w < an.windows ? an.w : an.windows;
        const int want = (int)(2 * ((nw + FINISH_BUDGET - 1) / FINISH_BUDGET));
        std::printf("candidates: %u windows in %d frames of at most %u windows a pass\n", nw, finishing_frames, (u32)FINISH_BUDGET);
        CHECK(finishing_frames >= want - 1 && finishing_frames <= want + 1, "the candidates took %d frames, not about %d", finishing_frames, want);
        CHECK(sl.n > 0 || st.p.mode != EQUAL, "no slices after the analysis");
    }
    std::printf("analysis: %u frames read in %d display frames, at most %u words a frame; %u candidates\n", an.frames, took + 21, max_words, an.ncand);
    CHECK(max_words <= (u32)BUDGET, "a frame read %u words", max_words);
    CHECK(wild == 0, "%ld reads outside the stood-in memory", wild);

    // ---- transient at the most sensitivity: every hit
    set(1, 50, 100);
    frame();
    {
        u32 found = 0;
        std::vector<int> taken(hits.size(), 0);
        for (u32 k = 0; k < sl.n; ++k) { const int h = cut_ok(sl.start[k], hits); if (h >= 0) taken[h] = 1; }
        for (size_t i = 0; i < hits.size(); ++i) found += taken[i];
        u32 stray = 0;
        for (u32 k = 1; k < sl.n; ++k) if (cut_ok(sl.start[k], hits) < 0) ++stray;
        std::printf("transient, sensitivity 100: %u slices; %u of %zu hits found (only 24 slices fit), %u stray cuts\n", sl.n, found, hits.size(), stray);
        CHECK(stray == 0, "%u cuts on no hit", stray);
        CHECK(sl.n == 24, "%u slices", sl.n);
    }
    // a shorter loop whose hits all fit: 2 bars, 16 hits
    std::vector<Hit> hits2 = beat(2, 3000);
    {
        const u32 F2 = 2 * 8 * 11025 + 5000;
        make_loop(F2, hits2);
        setup_unit(1, F2, 1);
        until_ready();
        u32 found = 0, stray = 0;
        std::vector<int> taken(hits2.size(), 0);
        for (u32 k = 0; k < sl.n; ++k) { const int h = cut_ok(sl.start[k], hits2); if (h >= 0) taken[h] = 1; else if (k) ++stray; }
        for (size_t i = 0; i < hits2.size(); ++i) found += taken[i];
        if (std::getenv("CHOP_DEBUG")) {
            for (u32 i = 0; i < an.ncand; ++i) std::printf("  cand frame %u str %u\n", an.cpos[i], an.cstr[i]);
            for (auto& h : hits2) std::printf("  hit %u kind %d win %u env %u %u %u %u\n", h.at, h.kind, h.at / WIN, an.env[h.at / WIN - 1], an.env[h.at / WIN], an.env[h.at / WIN + 1], an.env[h.at / WIN + 2]);
        }
        std::printf("transient on 2 bars, sensitivity 100: %u slices, %u of %zu hits, %u stray\n", sl.n, found, hits2.size(), stray);
        CHECK(found == hits2.size() && stray == 0, "found %u of %zu, %u stray", found, hits2.size(), stray);
        // fewer at less sensitivity; the kicks and snares outlast the hats
        u32 prev = 99, mono_ok = 1;
        for (int s = 100; s >= 0; s -= 10) {
            set(1, (int)st.p.sens, s); frame();
            if (sl.n > prev) mono_ok = 0;
            prev = sl.n;
            if (s == 50) {
                u32 hats = 0, loud = 0;
                for (u32 k = 1; k < sl.n; ++k) { const int h = cut_ok(sl.start[k], hits2); if (h >= 0) (hits2[h].kind == HAT ? hats : loud)++; }
                std::printf("transient, sensitivity 50: %u slices (%u kick/snare cuts, %u hat cuts)\n", sl.n, loud, hats);
                CHECK(loud >= 6 && hats <= 2, "sensitivity 50 kept %u loud, %u hats", loud, hats);
            }
        }
        CHECK(mono_ok, "more slices at a lower sensitivity");
        std::printf("transient, sensitivity 0: %u slices\n", sl.n);
        set(1, 0, 100); frame();
    }

    // ---- equal
    set(0, 0, EQUAL); frame();
    for (int n = 2; n <= 24; ++n) {
        set(1, (int)st.p.parts, n); frame();
        CHECK(sl.n == (u32)n, "equal %d gave %u", n, sl.n);
        for (u32 k = 1; k < sl.n; ++k) {
            const u32 len = sl.start[k] - sl.start[k - 1], want = an.frames / (u32)n;
            CHECK(len + 1 >= want && len <= want + 1, "equal %d: part %u is %u frames, not %u", n, k, len, want);
        }
    }
    set(1, 24, 8); frame();
    picture("page-equal8.ppm");

    // ---- lazy: zero crossings, gaps
    set(0, EQUAL, LAZY); frame();
    set(1, 100, 100); frame();
    for (int g = 20; g <= 500; g += 60) {
        turn(1, (g - (int)st.p.gap_ms) / 10, 1); frame();
        const u32 gap = frames_of_ms(st.p.gap_ms);
        u32 on_zero = 0;
        for (u32 k = 1; k < sl.n; ++k) {
            const u32 c = sl.start[k];
            const int a = (int16_t)(sample[c - 1] & 0xffff) + (int16_t)(sample[c - 1] >> 16), b = (int16_t)(sample[c] & 0xffff) + (int16_t)(sample[c] >> 16);
            if ((a >= 0) != (b >= 0)) ++on_zero;
            CHECK(c - sl.start[k - 1] + 2 * WIN >= gap, "lazy gap %u: cuts %u frames apart", st.p.gap_ms, c - sl.start[k - 1]);
        }
        CHECK(on_zero == sl.n - 1, "lazy gap %u: %u of %u cuts on a zero crossing", st.p.gap_ms, on_zero, sl.n - 1);
        if (g == 80) std::printf("lazy, gap %u ms: %u slices, %u cuts on zero crossings\n", st.p.gap_ms, sl.n, on_zero);
    }
    turn(1, (100 - (int)st.p.gap_ms) / 10, 1); frame();
    std::printf("lazy, gap 100 ms: %u slices\n", sl.n);

    // ---- mapping
    set(0, LAZY, TRANSIENT); frame();
    set(1, 100, 100); frame();
    set(2, (int)st.p.map, ORDER); frame();
    for (int k = 0; k < 24; ++k) CHECK(key_slice[k] == (k < (int)sl.n ? k : -1), "order: key %d slice %d", k, key_slice[k]);
    set(2, ORDER, ORDER_WRAP); frame();
    for (int k = 0; k < 24; ++k) CHECK(key_slice[k] == k % (int)sl.n, "wrap: key %d slice %d", k, key_slice[k]);
    set(2, ORDER_WRAP, KIT); frame();
    {
        auto kind_of = [&](int key) { const int s = key_slice[key]; if (s < 0) return -1; const int h = cut_ok(sl.start[s], hits2); return h < 0 ? (s == 0 ? (int)KICK : -1) : (int)hits2[h].kind; };
        std::printf("kit: keys 1,2 %s %s; 3,4 %s %s; 9,11 %s %s\n", kind_of(0) == KICK ? "kick" : "?", kind_of(1) == KICK ? "kick" : "?",
                    kind_of(2) == SNARE ? "snare" : "?", kind_of(3) == SNARE ? "snare" : "?", kind_of(8) == HAT ? "hat" : "?", kind_of(10) == HAT ? "hat" : "?");
        if (std::getenv("CHOP_DEBUG")) { Features f; features(an, sl, f); for (u32 s = 0; s < sl.n; ++s) { const int h = cut_ok(sl.start[s], hits2); std::printf("  slice %u kind %d bright %u strong %u len %u\n", s, h < 0 ? -1 : (int)hits2[h].kind, f.bright[s], f.strong[s], f.len[s]); } }
        CHECK(kind_of(0) == KICK && kind_of(1) == KICK, "kit: kicks %d %d", kind_of(0), kind_of(1));
        CHECK(kind_of(2) == SNARE && kind_of(3) == SNARE, "kit: snares %d %d", kind_of(2), kind_of(3));
        CHECK(kind_of(8) == HAT && kind_of(10) == HAT, "kit: hats %d %d", kind_of(8), kind_of(10));
        int seen[24] = {};
        for (int k = 0; k < 24; ++k) if (key_slice[k] >= 0) ++seen[key_slice[k]];
        for (u32 s = 0; s < sl.n; ++s) CHECK(seen[s] == 1, "kit: slice %u on %d keys", s, seen[s]);
    }
    picture("page-kit.ppm");

    // ---- apply, order, undo
    {
        u32 before[8][8 * 24];
        for (u32 s = 0; s < 8; ++s) for (u32 i = 0; i < 8 * 24; ++i) before[s][i] = words_mem[KEY_TABLE + s * KIT_BYTES + i * 4];
        set(2, KIT, ORDER_WRAP); frame();
        writes_seen = 0;
        press(3); frame();
        std::printf("apply: %ld words written, %ld out of order, %ld at a wrong place\n", writes_seen, bad_order, wrong_place);
        CHECK(writes_seen > 0 && writes_seen <= 48 && bad_order == 0 && wrong_place == 0, "apply wrote %ld (%ld out of order, %ld misplaced)", writes_seen, bad_order, wrong_place);
        u32 changed_other = 0;
        for (u32 s = 0; s < 8; ++s) for (u32 i = 0; i < 8 * 24; ++i) {
            const u32 off = (i & 7) * 4, now_v = words_mem[KEY_TABLE + s * KIT_BYTES + i * 4];
            if (now_v != before[s][i] && !(s == 1 && (off == 4 || off == 8))) ++changed_other;
        }
        CHECK(changed_other == 0, "%u words other than slot 2's starts and ends changed", changed_other);
        for (int k = 0; k < 24; ++k) {
            const u32 s0 = rec(1, k, 4), e0 = rec(1, k, 8), sidx = (u32)key_slice[k];
            CHECK(s0 == q31_of_frame(sl.start[sidx]) && e0 == q31_of_frame(sl.end[sidx]), "key %d: %08x-%08x", k, s0, e0);
            CHECK(e0 - s0 >= (u32)MIN_Q, "key %d shorter than MIN_Q", k);
        }
        // a second apply in another mode keeps the first originals
        set(0, TRANSIENT, EQUAL); frame();
        press(3); frame();
        CHECK(bad_order == 0, "second apply out of order");
        writes_seen = 0;
        {   // v3: as on the unit: shift's own down, the knob's press with no shift flag, shift's up
            Op1fInput sh = {}; sh.bytes = sizeof sh; sh.mine = 1; sh.what = OP1F_DOWN; sh.key = 38; veffchop_input(&sh);
            press(3, 0);
            sh.what = OP1F_UP; veffchop_input(&sh);
        }
        frame();
        u32 differ = 0;
        for (u32 s = 0; s < 8; ++s) for (u32 i = 0; i < 8 * 24; ++i) if (words_mem[KEY_TABLE + s * KIT_BYTES + i * 4] != before[s][i]) ++differ;
        std::printf("undo: %ld words written, %u words differ from the kit's own\n", writes_seen, differ);
        CHECK(differ == 0 && bad_order == 0, "undo left %u words changed", differ);
        writes_seen = 0;
        press(3, 1); frame();
        CHECK(writes_seen == 0, "a second undo wrote");
        // the order logic on its own: a pair moved wholly past the old end, and wholly before the old start
        u32 oldp[48], newp[48];
        for (int k = 0; k < 24; ++k) { oldp[2 * k] = 0x10000000u; oldp[2 * k + 1] = 0x20000000u; newp[2 * k] = k & 1 ? 0x30000000u : 0x01000000u; newp[2 * k + 1] = k & 1 ? 0x40000000u : 0x05000000u; }
        Write w[48];
        const int n = plan(oldp, newp, w);
        u32 cur[48]; for (int i = 0; i < 48; ++i) cur[i] = oldp[i];
        int bad = 0;
        for (int i = 0; i < n; ++i) { cur[2 * w[i].key + w[i].which] = w[i].value; if (cur[2 * w[i].key] > cur[2 * w[i].key + 1]) ++bad; }
        CHECK(n == 48 && bad == 0, "plan: %d writes, %d out of order", n, bad);
    }

    // ---- a press while the sample is being replaced writes nothing
    {
        set(0, EQUAL, TRANSIENT); frame();
        words_mem[OBJ_AT + OFF_WORDS] += 2;              // a load in flight
        writes_seen = 0;
        press(3); frame();
        CHECK(writes_seen == 0, "a press during a load wrote %ld words", writes_seen);
        CHECK(st.state != READY, "a changed sample still READY");
        words_mem[OBJ_AT + OFF_WORDS] -= 2;
        until_ready();
    }

    // ---- the page: a struck key, the play marker running
    set(2, (int)st.p.map, KIT); frame();
    strike(2);
    for (int i = 0; i < 6; ++i) frame();
    picture("page-transient-struck.ppm");
    set(2, KIT, ORDER_WRAP); frame();
    set(1, 100, 60); frame();
    strike(5);
    for (int i = 0; i < 3; ++i) frame();
    picture("page-transient.ppm");
    frame(1);
    picture("page-shift.ppm");

    // ---- v6: a mono sample (a halfword a frame) is analysed and applied against the mono capacity, 529200 frames;
    // then a strange stereo byte is refused, and a stereo sample analysed again
    {
        const u32 F = (u32)sample.size();
        setup_unit(1, F, 0);
        until_ready();
        CHECK(st.state == READY && st.at.stereo == 0 && an.frames == F && !an.stereo, "mono: state %u why %u frames %u", st.state, st.why, an.frames);
        for (int i = 0; i < 3; ++i) frame();
        writes_seen = 0; press(3); frame();
        CHECK(writes_seen > 0, "mono: a press wrote nothing");
        const int k0 = key_slice[0];
        CHECK(k0 >= 0 && rec(1, 0, 4) == q31_of_frame(sl.start[k0], 529200u) && rec(1, 0, 8) == q31_of_frame(sl.end[k0], 529200u),
              "mono: key 1 %08x-%08x, not its slice's Q31 of 529200", rec(1, 0, 4), rec(1, 0, 8));
        std::printf("mono: %u frames, %u slices, key 1 %08x-%08x\n", F, sl.n, rec(1, 0, 4), rec(1, 0, 8));
        picture("page-mono.ppm");
        setup_unit(5, F, 1);
        until_ready();
        CHECK(st.state == READY && st.at.slot == 5, "slot 6 not analysed");
        words_mem[CUR_SLOT] = 9;
        frame();
        CHECK(st.state == REFUSED && st.why == NO_SLOT, "slot 9 not refused");
    }
    // ---- v5: a kit being loaded: a stereo sample's words with its stereo byte still 0, then no sample for a few frames,
    // then the stereo byte set: never refused as mono or as no sample, analysed once it stays put
    {
        const u32 F = (u32)sample.size();
        setup_unit(2, F, 1);
        until_ready();
        u32 refused_seen = 0;
        setup_unit(2, 700000, 1); stereo_byte = 0;                // 1,400,008 words, the byte 0, for less than half a second
        for (int i = 0; i < 10; ++i) { frame(); if (st.state == REFUSED) ++refused_seen; }
        setup_unit(2, 0, 1);                                       // the words 8 for a moment
        for (int i = 0; i < 5; ++i) { frame(); if (st.state == REFUSED) ++refused_seen; }
        setup_unit(2, F, 1);
        until_ready();
        CHECK(refused_seen == 0 && st.state == READY && st.at.slot == 2, "a reload was judged: refused %u times, state %u why %u", refused_seen, st.state, st.why);
    }
    // ---- v6: the drop through the base: the key's down taken, op1f_sample_drop on its up with the drum place and ask 0,
    // the frame waits for `done`, then the sample is analysed again with the base's `dropped` as its length
    {
        const u32 F = (u32)sample.size();
        setup_unit(2, F, 1);
        until_ready();
        Op1fInput e = {}; e.bytes = 36; e.mine = 1; e.key = 11;
        e.what = OP1F_DOWN;
        CHECK(veffchop_input(&e) == 1 && drop_calls == 0, "the drop key's down not taken, or asked at the down");
        e.what = OP1F_UP;
        CHECK(veffchop_input(&e) == 1 && drop_calls == 1 && drop_place_seen == shown_where, "asked %u into %u", drop_calls, drop_place_seen);
        frame();
        CHECK(drop_ticket == 7, "the ticket not waited for");
        picture("page-dropping.ppm");
        mock_dropped = F / 2; mock_done = 7; mock_answer = (int)(F / 2);
        frame();
        CHECK(drop_ticket == 0 && drops_done == 1, "the end not seen");
        until_ready();
        CHECK(st.state == READY && an.frames == F / 2, "not analysed again with the dropped length: %u", an.frames);
        e.what = OP1F_DOWN; e.shift = 1;
        CHECK(veffchop_input(&e) == 0, "shift and drop taken");
        e.shift = 0; mock_drop_answer = OP1F_EMPTY;
        e.what = OP1F_DOWN; veffchop_input(&e); e.what = OP1F_UP; veffchop_input(&e);
        frame();
        CHECK(drop_ticket == 0 && drops_refused == 1 && drop_reason == OP1F_EMPTY, "a refusal at once not seen");
        picture("page-nothing-lifted.ppm");
        mock_dropped = 0; mock_done = 0; mock_drop_answer = 7;
    }
    // ---- v7: LAZY, the taps (lazy_tap.h): the parts, then the page on a stood-in unit
    {
        CHECK(lazy::frame_of_position(0x157ef7ffu) == 176094u && lazy::frame_of_position(0x1196efffu) == 144093u, "position to frame");
        u32 keys[8] = {4, 0, 0, 3, 0, 0, 0, 0}, states[8] = {1, 0, 2, 1, 0, 0, 0, 0}, pos[8] = {9, 50, 70, 5, 0, 0, 0, 0};
        CHECK(lazy::playing(keys, states, pos, 0) == 2, "the voice on key 1: %d", lazy::playing(keys, states, pos, 0));
        CHECK(lazy::playing(keys, states, pos, 7) == -1, "no voice on key 8");
        lazy::Taps t; lazy::clear(t);
        CHECK(lazy::add(t, 5000, 100000, 882) && lazy::add(t, 2000, 100000, 882) && !lazy::add(t, 2500, 100000, 882) &&
              !lazy::add(t, 100, 100000, 882) && !lazy::add(t, 99500, 100000, 882) && t.n == 2 && t.at[0] == 2000 && t.at[1] == 5000, "taps");
        for (u32 k = 0; k < 40; ++k) lazy::add(t, 10000 + k * 2000, 1000000, 882);
        CHECK(t.n == 23, "at most 23 cuts: %u", t.n);
        lazy::Taps u; lazy::clear(u); lazy::add(u, 3000, 9000, 882);
        Slices z; lazy::slices(u, 9000, z);
        CHECK(z.n == 2 && z.start[0] == 0 && z.end[0] == 2999 && z.start[1] == 3000 && z.end[1] == 8999, "slices of the taps");

        const u32 F = (u32)sample.size();
        setup_unit(3, F, 1);
        for (u32 v = 0; v < 8; ++v) { words_mem[VOICE_KEYS + 4 * v] = 9; words_mem[VOICE0 + v * 0x38 + 0x18] = 0; words_mem[VOICE0 + v * 0x38 + 0x2c] = 0; }
        until_ready();
        turn(0, (int)LAZY - (int)st.p.mode);                         // -> LAZY
        for (int i = 0; i < 3; ++i) frame();
        CHECK(st.p.mode == LAZY, "not LAZY");
        press(0); frame();
        CHECK(st.armed && rec(3, 0, 4) == 0 && rec(3, 0, 8) == q31_of_frame(F - 1, 882000u), "armed: key 1 %08x-%08x", rec(3, 0, 4), rec(3, 0, 8));
        picture("page-lazy-armed.ppm");
        Op1fInput e = {}; e.bytes = 36; e.mine = 1; e.what = OP1F_DOWN; e.key = 13;
        CHECK(veffchop_input(&e) == 0, "a key taken while key 1 does not play");
        words_mem[VOICE_KEYS + 4 * 5] = 0; words_mem[VOICE0 + 5 * 0x38 + 0x2c] = 1; words_mem[VOICE0 + 5 * 0x38 + 0x18] = 60000u << 11;
        frame();
        CHECK(st.playing && st.play_frame == 60000, "key 1's voice not followed: %u %u", st.playing, st.play_frame);
        CHECK(veffchop_input(&e) == 1, "a tap not taken while key 1 plays");
        e.key = LAZY_KEY; CHECK(veffchop_input(&e) == 0, "key 1 itself taken"); e.key = 13;
        frame();
        words_mem[VOICE0 + 5 * 0x38 + 0x18] = 60300u << 11; veffchop_input(&e); frame();     // too close: left out
        words_mem[VOICE0 + 5 * 0x38 + 0x18] = 150000u << 11; press(1); frame();              // knob 2 taps too
        for (int i = 0; i < 3; ++i) frame();
        CHECK(st.taps.n == 2 && sl.n == 3 && sl.start[1] == st.taps.at[0] && st.taps.at[0] + 2u * WIN >= 60000 && st.taps.at[0] <= 60000 + 2u * WIN,
              "taps %u slices %u first %u", st.taps.n, sl.n, st.taps.n ? st.taps.at[0] : 0);
        picture("page-lazy-taps.ppm");
        press(3); frame();
        CHECK(!st.armed && st.note == N_APPLIED && rec(3, 0, 4) == q31_of_frame(sl.start[key_slice[0]], 882000u), "applied: armed %u", st.armed);
        press(3, 1); frame();
        CHECK(st.note == N_UNDONE, "undo after a lazy apply");
        press(0); frame(); CHECK(st.armed, "armed again");
        press(0); frame(); CHECK(!st.armed && st.note == N_UNDONE, "disarmed: the kit's own back");
        words_mem[VOICE0 + 5 * 0x38 + 0x2c] = 0;
        turn(0, -1); for (int i = 0; i < 3; ++i) frame();
    }
    // ---- v8: EDIT, COPY and PASTE (edit_paste.h): the parts, then the page
    {
        CHECK(edit::span(882000, 0) == 882000 && edit::span(882000, 1) == 441000 && edit::span(882000, 12) == 220 && edit::span(100, 3) == 100, "spans");
        CHECK(edit::view_first(1000, 200, 50) == 0 && edit::view_first(1000, 200, 990) == 800 && edit::view_first(1000, 200, 500) == 400, "view");
        CHECK(edit::step(882000, 228) == 3868 && edit::step(220, 228) == 1, "steps: %u", edit::step(882000, 228));
        CHECK(edit::moved(100, -5, 30, 1000) == 0 && edit::moved(100, 5, 300, 1000) == 1000 && edit::moved(100, 2, 10, 1000) == 120, "moved");
        u32 a = 5000, b = 5100; edit::in_order(a, b, 100000, 1); CHECK(a == 5000 && b == 5000 + edit::LEAST_LENGTH, "in order, the end moved: %u %u", a, b);
        a = 5000; b = 5100; edit::in_order(a, b, 100000, 0); CHECK(b == 5100 && a == 5100 - edit::LEAST_LENGTH, "in order, the start moved: %u %u", a, b);
        u32 at = 0, missing = 0;
        CHECK(edit::place(800000, 90000, 1, at, missing) == 0 && missing == 800064 + 90000 - 881996, "stereo over: missing %u", missing);
        CHECK(edit::place(800000, 80000, 1, at, missing) == 1 && at == 800064, "stereo fits at %u", at);
        CHECK(edit::place(500001, 29000, 0, at, missing) == 1 && at == 500066 && !(at & 1), "mono at an even frame %u", at);
        CHECK(edit::place(500000, 40000, 0, at, missing) == 0 && missing == 500064 + 40000 - 529200, "mono over: missing %u", missing);
        u32 ends[3] = {100, 9000, 400};
        CHECK(edit::used(5000, ends, 3, 0) == 9001 && edit::used(5000, ends, 3, 12000) == 12000 && edit::used(20000, ends, 3, 0) == 20000, "used");
        short in[2] = {1000, -3000}, out[2] = {0, 0};
        edit::convert(in, 1, 0, out); CHECK(out[0] == -1000, "stereo to mono: %d", out[0]);
        edit::convert(in, 0, 1, out); CHECK(out[0] == 1000 && out[1] == 1000, "mono to stereo");
        CHECK(edit::clip_room(1) == 441000 && edit::clip_room(0) == 882000, "the copy's room");

        const u32 F = (u32)sample.size();
        setup_unit(4, F, 1);
        for (u32 v = 0; v < 8; ++v) { words_mem[VOICE_KEYS + 4 * v] = 30; words_mem[VOICE0 + v * 0x38 + 0x18] = 0; words_mem[VOICE0 + v * 0x38 + 0x2c] = 0; }
        until_ready();
        st.struck_valid = 0;
        frame(1); picture("page-shift-hints.ppm");                  // v10: the shift-presses said on the page
        press(2, 1); frame();
        CHECK(st.editing && st.ed_key == 0, "EDIT without a key struck: key 1 (%u %u)", st.editing, st.ed_key);
        frame(1); picture("page-edit-shift-hints.ppm");
        press(2, 1); frame();
        CHECK(!st.editing, "left");
        strike(2); frame();
        frame(1); picture("page-shift-hints-key.ppm");
        press(2, 1); frame();
        CHECK(st.editing && st.ed_key == 2 && st.ed_start == frame_of_q31(rec(4, 2, 4)) && st.ed_end == frame_of_q31(rec(4, 2, 8)),
              "EDIT: key %u %u-%u", st.ed_key, st.ed_start, st.ed_end);
        // v10: shift's down came to us and its up never does (the unit's): the frame's word of shift lets go of it, and a
        // turn with shift moves in EDIT too
        { Op1fInput sh = {}; sh.bytes = 36; sh.mine = 1; sh.what = OP1F_DOWN; sh.key = 38; veffchop_input(&sh); }
        frame();
        CHECK(shift_held == 0, "shift still held after a frame without it");
        { const u32 s0 = st.ed_start; turn(0, 1, 1); frame(); CHECK(st.ed_start != s0, "a turn with shift moved nothing in EDIT"); turn(0, -1, 1); frame(); }
        const u32 s_before = st.ed_start, step0 = edit::step(edit::span(st.ed_total, 0), COLS);
        turn(0, 3); frame();
        CHECK(st.ed_start == s_before + 3 * step0 && rec(4, 2, 4) == q31_of_frame(st.ed_start, 882000u) && bad_order == 0, "START: %u", st.ed_start);
        turn(2, 6); max_words = 0;
        for (int i = 0; i < 12; ++i) frame();
        CHECK(st.ed_zoom == 6 && st.zc_next == COLS && st.zc_span == edit::span(st.ed_total, 6) && max_words <= (u32)BUDGET, "ZOOM: %u %u %u words %u", st.ed_zoom, st.zc_next, st.zc_span, max_words);
        turn(1, -2); frame();
        CHECK(rec(4, 2, 8) == q31_of_frame(st.ed_end, 882000u), "END written");
        const u32 len = st.ed_end - st.ed_start;
        turn(3, 4); frame();
        CHECK(st.ed_end - st.ed_start == len && rec(4, 2, 4) == q31_of_frame(st.ed_start, 882000u), "NUDGE keeps the length");
        press(3); frame();
        CHECK(bad_order == 0 && wrong_place == 0, "SNAP");
        words_mem[VOICE_KEYS + 4 * 3] = 2; words_mem[VOICE0 + 3 * 0x38 + 0x2c] = 1; words_mem[VOICE0 + 3 * 0x38 + 0x18] = (st.ed_start + 10) << 11;
        frame();
        CHECK(st.ed_playing && st.ed_play_frame == st.ed_start + 10, "the key's play line");
        words_mem[VOICE0 + 3 * 0x38 + 0x2c] = 0;
        picture("page-edit.ppm");
        // COPY key 3, then PASTE onto key 6
        const u32 c0 = st.ed_start, cn = st.ed_end - st.ed_start + 1;
        press(0, 1);
        for (int i = 0; i < 40 && (st.copy_state || !st.clip_frames); ++i) frame();
        CHECK(st.clip_frames == cn && st.clip_stereo == 1 && clip_memory[0] == (short)(sample[c0] & 0xffff) && clip_memory[2 * cn - 1] == (short)(sample[c0 + cn - 1] >> 16),
              "COPY: %u frames", st.clip_frames);
        strike(5); frame();
        CHECK(st.ed_key == 5, "another key struck while editing: %u", st.ed_key);
        u32 kends[KEYS]; for (int k = 0; k < KEYS; ++k) kends[k] = frame_of_q31(rec(4, (u32)k, 8));
        const u32 used_want = edit::used(F, kends, KEYS, 0);
        press(1, 1); frame();
        for (int i = 0; i < 400 && st.paste_state == 1; ++i) frame();
        CHECK(st.paste_state == 2 && changed_calls == 1 && changed_place == (u32)OP1F_PLACE_DRUM + 4u && pool_wild == 0, "PASTE: state %u calls %u wild %ld", st.paste_state, changed_calls, pool_wild);
        u32 pat = 0, pmiss = 0; edit::place(used_want, cn, 1, pat, pmiss);
        CHECK(st.paste_at == pat && rec(4, 5, 4) == q31_of_frame(pat, 882000u) && pool_mem[DATA_AT + 16 + pat * 4] == sample[c0] &&
              pool_mem[DATA_AT + 16 + (pat + cn - 1) * 4] == sample[c0 + cn - 1], "the pasted frames and key 6's start (at %u)", st.paste_at);
        mock_done = 9; mock_answer = 0; frame();
        CHECK(st.paste_state == 0 && st.ed_note == EN_KEPT, "kept: note %u", st.ed_note);
        picture("page-pasted.ppm");
        // a second paste goes behind the first
        press(1, 1); frame();
        for (int i = 0; i < 400 && st.paste_state == 1; ++i) frame();
        CHECK(st.paste_at >= pat + cn, "the second paste behind the first: %u", st.paste_at);
        mock_done = 0;
        frame();
        // v10: after a drop of the base's, the material is the cut (`dropped`), not the words nor the keys' stale ends
        mock_dropped = 20000; mock_done = 0;
        st.at.obj = 0; until_ready();                                // seen anew with the dropped length, as after a drop
        st.paste_end[4] = 0;
        { Where w; locate(w); CHECK(material_end(w) == 20000, "the material after a drop: %u", material_end(w)); }
        if (!st.editing) { strike(5); frame(); press(2, 1); frame(); }
        CHECK(st.editing, "EDIT again after the drop");
        st.paste_state = 0;                                          // the second paste's keep is not answered here
        press(1, 1); frame();
        CHECK(st.paste_state == 1 && st.paste_at == 20064, "a paste after a drop goes behind the cut: %u", st.paste_at);
        for (int i = 0; i < 400 && st.paste_state == 1; ++i) frame();
        mock_dropped = 0; st.at.obj = 0; until_ready();
        if (!st.editing) { strike(5); frame(); press(2, 1); frame(); }
        // too large: a copy that does not fit says how much is missing
        st.clip_frames = 800000; st.paste_state = 0;
        press(1, 1); frame();
        CHECK(st.ed_note == EN_MISSING && st.ed_missing > 0, "too large: note %u missing %u", st.ed_note, st.ed_missing);
        picture("page-paste-missing.ppm");
        // stereo copy into a mono slot: averaged, two frames a word
        st.clip_frames = 4; clip_memory[0] = 100; clip_memory[1] = 300; clip_memory[2] = -50; clip_memory[3] = -150;
        clip_memory[4] = 7; clip_memory[5] = 9; clip_memory[6] = 1; clip_memory[7] = 1; st.clip_stereo = 1;
        press(3, 1); frame();                                       // UNDO leaves EDIT
        CHECK(!st.editing && st.note == N_UNDONE, "UNDO from EDIT");
        setup_unit(6, F, 0);
        for (u32 v = 0; v < 8; ++v) { words_mem[VOICE_KEYS + 4 * v] = 30; words_mem[VOICE0 + v * 0x38 + 0x18] = 0; words_mem[VOICE0 + v * 0x38 + 0x2c] = 0; }
        until_ready();
        strike(0); frame(); press(2, 1); frame();
        CHECK(st.editing && st.ed_stereo == 0, "EDIT on a mono slot");
        const u32 c_before = changed_calls;
        press(1, 1); frame();
        for (int i = 0; i < 20 && st.paste_state == 1; ++i) frame();
        const u32 w0 = pool_mem[DATA_AT + 16 + st.paste_at * 2], w1 = pool_mem[DATA_AT + 16 + st.paste_at * 2 + 4];
        CHECK(changed_calls == c_before + 1 && (short)(w0 & 0xffff) == 200 && (short)(w0 >> 16) == -100 && (short)(w1 & 0xffff) == 8 && (short)(w1 >> 16) == 1,
              "stereo into mono: %08x %08x", w0, w1);
        CHECK(rec(6, 0, 4) == q31_of_frame(st.paste_at, 529200u), "the mono key's start against 529200");
        press(2, 1); frame();
        CHECK(!st.editing, "shift and knob 3 leaves EDIT");
        mock_done = 0;
    }
    // ---- a long sample (the most a slot holds) within the budget
    {
        max_words = 0;
        make_loop(860000, beat(10, 1000));
        setup_unit(0, 860000, 1);
        const int t = until_ready(4000);
        std::printf("860000 frames: analysed in %d display frames (%.1f s at 60 frames a second), at most %u words a frame, %u slices\n",
                    t, t / 60.0, max_words, sl.n);
        CHECK(max_words <= (u32)BUDGET && st.state == READY, "long sample");
        CHECK(wild == 0, "%ld wild reads", wild);
    }

    std::printf("log:\n");
    for (size_t i = 0; i < log_lines.size() && i < 40; ++i) std::printf("  %s\n", log_lines[i].c_str());
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
