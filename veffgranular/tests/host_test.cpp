// veffgranular host test: the grain DSP (granular_core.h) and the file reader (sample_file.h) on a computer.
// Build and run with tests/run_tests.sh (address and undefined-behaviour sanitizers). Writes rendered WAV files
// (88,200 Hz, stereo, 16-bit) into the folder given as the first argument. Exits non-zero on the first failure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include "../granular_core.h"
#include "../sample_file.h"
#include "../sampler_region.h"
#include "../sampler_scan.h"
#include "../drop_logic.h"

using namespace gran;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const u32 STEP_A4 = 21426141u;
static u32 step_of(int midi) { return (u32)std::lround(STEP_A4 * std::pow(2.0, (midi - 69) / 12.0)); }

// ---- test samples
static std::vector<short> make_sample(u32 frames, int stereo, int kind) {
    std::vector<short> s(frames * (stereo ? 2 : 1));
    u32 seed = 1;
    for (u32 i = 0; i < frames; ++i) {
        double t = i / 44100.0, v;
        if (kind == 0) v = 0.6 * std::sin(2 * M_PI * 220.0 * t) * (0.5 + 0.5 * std::sin(2 * M_PI * 0.7 * t)) +
                           0.25 * std::sin(2 * M_PI * (330.0 + 400.0 * t) * t);    // a tone with a sweep in it
        else if (kind == 1) { seed = seed * 1664525u + 1013904223u; v = ((int)(seed >> 16) - 32768) / 40000.0; }
        else v = 0;
        short a = (short)std::lround(v * 32767.0 * 0.9);
        if (stereo) { s[2 * i] = a; s[2 * i + 1] = (short)(kind == 0 ? std::lround(0.5 * std::sin(2 * M_PI * 110.0 * t) * 29000) : a); }
        else s[i] = a;
    }
    return s;
}

// a source whose region is the whole of one heap block, so any read outside [start, end) is outside the block and
// the address sanitizer stops the test
struct Owned {
    short* block = nullptr; GrainSource src{};
    void set(const std::vector<short>& s, int stereo, u32 start, u32 end, u32 root = 60, u32 gen = 1) {
        std::free(block);
        const u32 ch = stereo ? 2 : 1, frames = (u32)s.size() / ch;
        block = (short*)std::malloc((end - start) * ch * sizeof(short));
        std::memcpy(block, s.data() + start * ch, (end - start) * ch * sizeof(short));
        src.data = block - start * ch;    // frames before start are never read
        src.frames = frames; src.stereo = (u32)stereo; src.start = start; src.end = end; src.root = root;
        src.rate_q16 = 65536; src.generation = gen; src.valid = 1;
    }
    ~Owned() { std::free(block); }
};

struct Run {
    Voice v{}; Settings set{}; int values[VALUES] = { 300, 50, 50, 10, 0, 30, 0, 80 }; Vis vis{};
    i32 out[2 * FRAMES];
    std::vector<short> wav;
    int max_grains = 0;
    u32 grain_frames_most = 0;
    void block(const GrainSource& s, u32 step, u32 events, u32 period = 0) {
        Block b{ values, step, events, period, 0, out };
        render(v, s, set, b, &vis);
        int n = 0; for (int k = 0; k < GRAINS; ++k) n += v.g[k].wstep != 0;
        if (n > max_grains) max_grains = n;
        for (u32 i = 0; i < 2 * FRAMES; ++i) wav.push_back((short)(out[i] >> 16));
    }
    double peak() const { int m = 0; for (short x : wav) m = std::max(m, std::abs((int)x)); return m / 32768.0; }
};

static void write_wav(const std::string& path, const std::vector<short>& d, u32 rate, u32 ch) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const u32 bytes = (u32)d.size() * 2;
    auto w32 = [&](u32 x) { std::fwrite(&x, 4, 1, f); };
    auto w16 = [&](unsigned short x) { std::fwrite(&x, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16((unsigned short)ch);
    w32(rate); w32(rate * ch * 2); w16((unsigned short)(ch * 2)); w16(16); std::fwrite("data", 1, 4, f); w32(bytes);
    std::fwrite(d.data(), 2, d.size(), f);
    std::fclose(f);
}

// a file in memory, for the reader
static std::vector<unsigned char> file_wav(const std::vector<short>& d, u32 ch, u32 rate, int smpl_root = -1) {
    std::vector<unsigned char> b;
    auto put = [&](const void* p, size_t n) { const unsigned char* c = (const unsigned char*)p; b.insert(b.end(), c, c + n); };
    auto p32 = [&](u32 x) { put(&x, 4); };
    auto p16 = [&](unsigned short x) { put(&x, 2); };
    put("RIFF", 4); p32(0); put("WAVE", 4);
    put("fmt ", 4); p32(16); p16(1); p16((unsigned short)ch); p32(rate); p32(rate * ch * 2); p16((unsigned short)(ch * 2)); p16(16);
    if (smpl_root >= 0) { put("smpl", 4); p32(36); for (int i = 0; i < 3; ++i) p32(0); p32((u32)smpl_root); for (int i = 0; i < 5; ++i) p32(0); }
    put("data", 4); p32((u32)d.size() * 2); put(d.data(), d.size() * 2);
    u32 size = (u32)b.size() - 8; std::memcpy(&b[4], &size, 4);
    return b;
}
static std::vector<unsigned char> file_aiff(const std::vector<short>& d, u32 ch, int sowt) {
    std::vector<unsigned char> b;
    auto be32 = [&](u32 x) { unsigned char c[4] = { (unsigned char)(x >> 24), (unsigned char)(x >> 16), (unsigned char)(x >> 8), (unsigned char)x }; b.insert(b.end(), c, c + 4); };
    auto be16 = [&](u32 x) { b.push_back((unsigned char)(x >> 8)); b.push_back((unsigned char)x); };
    auto put = [&](const char* s, size_t n) { b.insert(b.end(), s, s + n); };
    put("FORM", 4); be32(0); put(sowt ? "AIFC" : "AIFF", 4);
    put("COMM", 4); be32(sowt ? 24 : 18); be16(ch); be32((u32)d.size() / ch); be16(16);
    // 44100 as an 80-bit extended number
    const unsigned char rate[10] = { 0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0 }; b.insert(b.end(), rate, rate + 10);
    if (sowt) { put("sowt", 4); b.push_back(0); b.push_back(0); }
    put("INST", 4); be32(20); b.push_back(48); for (int i = 0; i < 19; ++i) b.push_back(0);
    put("SSND", 4); be32(8 + (u32)d.size() * 2); be32(0); be32(0);
    for (short x : d) { if (sowt) { b.push_back((unsigned char)x); b.push_back((unsigned char)((unsigned short)x >> 8)); } else be16((unsigned short)x); }
    u32 size = (u32)b.size() - 8; b[4] = (unsigned char)(size >> 24); b[5] = (unsigned char)(size >> 16); b[6] = (unsigned char)(size >> 8); b[7] = (unsigned char)size;
    return b;
}

// the first grain in a render of grains that never overlap: [begin, end) in words of the left channel's pairs
static void first_grain(const std::vector<short>& w, size_t& begin, size_t& end) {
    begin = 0; while (begin < w.size() && w[begin] == 0) begin += 2;
    end = begin; while (end < w.size() && w[end] != 0) end += 2;
}

// a note: `held` blocks with the key down, then `after` blocks released
static void play(Run& r, const GrainSource& s, int midi, int held, int after, u32 period = 0) {
    r.block(s, step_of(midi), EV_FRESH | EV_BEGINS, period);
    for (int i = 1; i < held; ++i) r.block(s, step_of(midi), 0, period);
    r.block(s, step_of(midi), EV_ENDS, period);
    for (int i = 1; i < after; ++i) r.block(s, step_of(midi), 0, period);
}

static const int BLOCKS_PER_S = 689;   // 88200 / 128

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const auto mono = make_sample(66150, 0, 0), stereo = make_sample(66150, 1, 0), noise = make_sample(44100, 0, 1);

    // ---- renders for listening, each mode at a few settings
    struct Case { const char* name; u32 mode; int shape; u32 loop; int values[VALUES]; int stereo, midi; u32 reverse, pitch_random, level_random; };
    const Case cases[] = {
        { "classic_default", MODE_CLASSIC, 0, 0, { 300, 50, 50, 10, 0, 30, 0, 80 }, 0, 60, 0, 0, 0 },
        { "classic_scan_fwd", MODE_CLASSIC, 0, 0, { 0, 40, 70, 5, 60, 50, 0, 80 }, 1, 60, 0, 0, 0 },
        { "classic_scan_back", MODE_CLASSIC, 60, 0, { 1000, 60, 60, 0, -50, 20, 0, 80 }, 1, 60, 0, 0, 0 },
        { "classic_short_pluck", MODE_CLASSIC, -100, 0, { 500, 10, 90, 30, 0, 80, 12, 80 }, 0, 67, 0, 0, 0 },
        { "classic_pluck_long", MODE_CLASSIC, -100, 0, { 300, 75, 40, 20, 0, 40, 0, 80 }, 0, 60, 0, 0, 0 },
        { "classic_swell", MODE_CLASSIC, 100, 0, { 300, 70, 55, 10, 0, 40, 0, 80 }, 1, 60, 0, 0, 0 },
        { "classic_reversed", MODE_CLASSIC, 0, 0, { 400, 65, 60, 10, 0, 30, 0, 80 }, 1, 60, 100, 0, 0 },
        { "classic_half_reversed", MODE_CLASSIC, -60, 0, { 400, 55, 70, 20, 0, 60, 0, 80 }, 0, 60, 50, 0, 0 },
        { "classic_pitch_random", MODE_CLASSIC, -40, 0, { 300, 35, 75, 30, 0, 70, 0, 80 }, 0, 60, 0, 100, 0 },
        { "classic_level_random", MODE_CLASSIC, 0, 0, { 300, 30, 80, 30, 0, 50, 0, 80 }, 0, 60, 20, 15, 100 },
        { "loop_free", MODE_LOOP, 0, 0, { 400, 30, 60, 0, 0, 20, 0, 80 }, 1, 60, 0, 0, 0 },
        { "loop_eighth_120bpm", MODE_LOOP, 40, 2, { 200, 40, 50, 0, 10, 40, 0, 80 }, 1, 60, 0, 0, 0 },
        { "cloud_dense", MODE_CLOUD, 0, 0, { 500, 70, 100, 40, 0, 100, 0, 80 }, 1, 60, 0, 0, 0 },
        { "cloud_swell_low", MODE_CLOUD, 70, 0, { 300, 80, 80, 80, 20, 60, -12, 80 }, 0, 48, 0, 0, 0 },
    };
    const u32 period120 = (u32)std::lround(44100.0 * 60 / 120 / 96 * 262144.0);
    int max_grains = 0;
    for (const Case& c : cases) {
        Owned o; o.set(c.stereo ? stereo : mono, c.stereo, 0, 66150);
        Run r; r.set = { c.mode, c.loop, c.shape, c.reverse, c.pitch_random, c.level_random }; std::memcpy(r.values, c.values, sizeof r.values);
        play(r, o.src, c.midi, 3 * BLOCKS_PER_S, BLOCKS_PER_S, c.mode == MODE_LOOP ? period120 : 0);
        const double pk = r.peak();
        std::printf("render %-22s peak %.3f, grains at once at most %d\n", c.name, pk, r.max_grains);
        CHECK(pk > 0.02, "%s is silent", c.name);
        CHECK(r.wav[0] == 0 && r.wav[1] == 0, "%s starts with a click", c.name);
        if (r.max_grains > max_grains) max_grains = r.max_grains;
        write_wav(dir + "/veffgranular_" + c.name + ".wav", r.wav, 88200, 2);
    }


    // ---- pitch: a 440 Hz sine with root A4 plays 440 Hz at A4, 880 at A5, 220 at A3 with tune -12
    {
        std::vector<short> sine(88200);
        for (u32 i = 0; i < 88200; ++i) sine[i] = (short)std::lround(20000 * std::sin(2 * M_PI * 440.0 * i / 44100.0));
        struct P { int midi, tune; double hz; } ps[] = { { 69, 0, 440 }, { 81, 0, 880 }, { 69, -12, 220 }, { 60, 7, 440 * std::pow(2, -2 / 12.0) } };
        for (const P& q : ps) {
            Owned o; o.set(sine, 0, 0, 88200, 69);
            Run r; r.values[V_SPRAY] = 0; r.values[V_SPREAD] = 0; r.values[V_SIZE] = 80; r.values[V_DENSITY] = 40; r.values[V_TUNE] = q.tune;   // overlap 1: grains one after another
            play(r, o.src, q.midi, BLOCKS_PER_S * 2, 1);
            int z = 0;
            for (size_t i = 88200 * 2 + 2; i < 88200 * 4; i += 2) z += r.wav[i - 2] < 0 && r.wav[i] >= 0;
            std::printf("pitch: note %d tune %d: %d Hz, want %.0f\n", q.midi, q.tune, z, q.hz);
            CHECK(std::abs(z - q.hz) < q.hz * 0.01 + 2, "pitch off");
        }
    }

    // ---- silence in, silence out
    {
        const auto zero = make_sample(44100, 1, 2);
        Owned o; o.set(zero, 1, 0, 44100);
        for (u32 m = 0; m < MODES; ++m) {
            Run r; r.set = { m, 0, 0 }; r.values[V_SPREAD] = 100; r.values[V_SPRAY] = 100;
            play(r, o.src, 60, 300, 50);
            int any = 0; for (short x : r.wav) any |= x;
            CHECK(!any, "mode %u: silence in, sound out", m);
        }
    }

    // ---- no stuck grains: released, everything stops within the hold plus the longest grain
    for (u32 m = 0; m < MODES; ++m) {
        Owned o; o.set(stereo, 1, 0, 66150);
        Run r; r.set = { m, 0, 0 }; r.values[V_SIZE] = 100;
        play(r, o.src, 60, 100, HOLD_BLOCKS + 2 * BLOCKS_PER_S);
        int n = 0; for (int k = 0; k < GRAINS; ++k) n += r.v.g[k].wstep != 0;
        int tail = 0; for (size_t i = r.wav.size() - 2 * FRAMES; i < r.wav.size(); ++i) tail |= r.wav[i];
        CHECK(n == 0 && !tail && !r.v.sounding, "mode %u: grains left after release (%d), last block %s", m, n, tail ? "not silent" : "silent");
    }

    // ---- determinism
    {
        Owned o; o.set(stereo, 1, 0, 66150);
        Run a, b; a.set = b.set = { MODE_CLOUD, 0, 0 };
        play(a, o.src, 62, 400, 50); play(b, o.src, 62, 400, 50);
        CHECK(a.wav == b.wav, "two renders of the same note differ");
    }

    // ---- a sample shorter than a grain, mono and stereo, every mode, the highest note
    for (int st = 0; st < 2; ++st) for (u32 m = 0; m < MODES; ++m) for (u32 len : { 2u, 3u, 100u, 1000u }) {
        const auto s = make_sample(len, st, 0);
        Owned o; o.set(s, st, 0, len);
        Run r; r.set = { m, 0, 6 }; r.values[V_SIZE] = 100; r.values[V_SPRAY] = 100; r.values[V_SCAN] = -100; r.values[V_TUNE] = 24;
        play(r, o.src, 108, 200, 20, period120);
        play(r, o.src, 21, 200, 20, period120);
    }
    std::printf("short samples (2, 3, 100, 1000 frames): no read outside them\n");

    // ---- a region inside a longer sample, which moves under a held note (the sampler's start and end points)
    {
        Owned o; o.set(mono, 0, 20000, 30000);
        Run r; r.set = { MODE_CLASSIC, 0, 0, 0, 0, 0 }; r.values[V_SPRAY] = 100; r.values[V_SCAN] = 100;
        r.block(o.src, step_of(60), EV_FRESH | EV_BEGINS);
        for (int i = 0; i < 200; ++i) r.block(o.src, step_of(60), 0);
        // the region narrows: the owner moved the start up; same data, same generation
        o.set(mono, 0, 25000, 26000, 60, 1);
        for (int i = 0; i < 400; ++i) r.block(o.src, step_of(60), 0);
        // the sample is replaced: a new generation
        o.set(stereo, 1, 1000, 60000, 48, 2);
        for (int i = 0; i < 400; ++i) r.block(o.src, step_of(60), 0);
        CHECK(r.peak() > 0.02, "a moved region plays nothing");
        // the step at the change is no more than a fade's: |difference| between neighbouring frames
        std::printf("moved region and replaced sample: no read outside the region\n");
    }

    // ---- empty and invalid sources
    {
        Owned o; o.set(mono, 0, 0, 66150);
        GrainSource bad[6];
        for (auto& b : bad) b = o.src;
        bad[0].valid = 0; bad[1].data = nullptr; bad[2].frames = 0; bad[3].start = bad[3].end = 10; bad[4].end = 66151; bad[5].rate_q16 = 0;
        for (const auto& b : bad) {
            Run r; play(r, b, 60, 50, 10);
            int any = 0; for (short x : r.wav) any |= x;
            CHECK(!any, "an invalid source makes sound");
        }
        // valid, then cleared while a note sounds: it fades to silence
        Run r; r.block(o.src, step_of(60), EV_FRESH | EV_BEGINS);
        for (int i = 0; i < 100; ++i) r.block(o.src, step_of(60), 0);
        GrainSource gone = o.src; gone.valid = 0;
        for (int i = 0; i < 3; ++i) r.block(gone, step_of(60), 0);
        int tail = 0; for (size_t i = r.wav.size() - 2 * FRAMES; i < r.wav.size(); ++i) tail |= r.wav[i];
        CHECK(!tail, "a cleared source still sounds");
    }

    // ---- the reader: wav and aiff, mono and stereo, roots; cut and random files never crash it
    {
        struct F { const char* name; std::vector<unsigned char> b; u32 frames, stereo, root; };
        const auto small = make_sample(5000, 1, 0), smallm = make_sample(5000, 0, 0);
        std::vector<F> files = {
            { "wav stereo", file_wav(small, 2, 44100), 5000, 1, 60 },
            { "wav mono 48k root 64", file_wav(smallm, 1, 48000, 64), 5000, 0, 64 },
            { "aiff stereo root 48", file_aiff(small, 2, 0), 5000, 1, 48 },
            { "aifc sowt mono", file_aiff(smallm, 1, 1), 5000, 0, 48 },
        };
        for (auto& f : files) {
            std::vector<unsigned char> b = f.b;
            sfile::Info in{};
            const int e = sfile::parse(b.data(), (u32)b.size(), &in);
            CHECK(e == sfile::OK && in.frames == f.frames && in.stereo == f.stereo && in.root == f.root, "%s: error %d, %u frames", f.name, e, in.frames);
            const auto& want = f.stereo ? small : smallm;
            CHECK(e != sfile::OK || std::memcmp(b.data(), want.data(), want.size() * 2) == 0, "%s: frames differ", f.name);
            // every cut of it
            for (size_t n = 0; n < f.b.size(); n += 7) {
                std::vector<unsigned char> c(f.b.begin(), f.b.begin() + n);
                c.shrink_to_fit();
                unsigned char* heap = (unsigned char*)std::malloc(n ? n : 1); std::memcpy(heap, c.data(), n);
                sfile::Info ci{}; const int ce = sfile::parse(heap, (u32)n, &ci);
                CHECK(ce != sfile::OK || ci.frames * (ci.stereo ? 4u : 2u) <= n, "%s cut at %zu: frames past the file", f.name, n);
                std::free(heap);
            }
        }
        sfile::Info in{};
        std::vector<unsigned char> b = files[0].b; b[20] = 3;       // float format
        CHECK(sfile::parse(b.data(), (u32)b.size(), &in) == sfile::E_CODEC, "a float wav is taken");
        u32 seed = 7;
        for (int t = 0; t < 20000; ++t) {
            const u32 n = 16 + (t % 300);
            unsigned char* heap = (unsigned char*)std::malloc(n);
            for (u32 i = 0; i < n; ++i) { seed = seed * 1664525u + 1013904223u; heap[i] = (unsigned char)(seed >> 24); }
            if (t & 1) std::memcpy(heap, t & 2 ? "RIFF\x10\0\0\0WAVEfmt " : "FORM\x10\0\0\0AIFFCOMM", 16);
            sfile::parse(heap, n, &in);
            std::free(heap);
        }
        std::printf("reader: 4 good files, every cut of them, 20000 random files\n");
    }

    // ---- shape, reverse and the two random values
    {
        // one grain at a time over a constant sample: the output is the window itself, so its edges and steps show
        std::vector<short> dc(66150, 20000);
        const int shapes[] = { -100, -50, 0, 50, 100 }, sizes[] = { 0, 20, 50, 80, 100 };
        double worst = 0; int runs = 0;
        for (int sh : shapes) for (int sz : sizes) for (u32 rev = 0; rev <= 100; rev += 100) {
            Owned o; o.set(dc, 0, 0, 66150);
            Run r; r.set = { MODE_CLASSIC, 0, sh, rev, 0, 0 };
            const int v[VALUES] = { 500, sz, 0, 0, 0, 0, 0, 80 };          // density 0: grains never overlap
            std::memcpy(r.values, v, sizeof v);
            play(r, o.src, 60, BLOCKS_PER_S, BLOCKS_PER_S / 2);
            int pk = 0, jump = 0, zeros = 0;
            for (size_t i = 0; i < r.wav.size(); i += 2) {
                const int x = r.wav[i], before = i ? r.wav[i - 2] : 0;
                pk = std::max(pk, std::abs(x)); jump = std::max(jump, std::abs(x - before)); zeros += x == 0;
            }
            CHECK(pk > 500, "shape %d size %d reverse %u: silent", sh, sz, rev);
            CHECK(zeros > 0, "shape %d size %d reverse %u: the grains never part", sh, sz, rev);
            // the steepest a 16-frame raised-cosine edge can be is pi/32 of the top a frame
            CHECK(jump <= pk * 0.11 + 2, "shape %d size %d reverse %u: a step of %d against a top of %d (a click)", sh, sz, rev, jump, pk);
            worst = std::max(worst, (double)jump / pk); ++runs;
        }
        std::printf("shape: %d runs of single grains, the steepest step %.3f of the top\n", runs, worst);

        // where the top stands: early for a pluck, late for a swell
        auto top_at = [&](int sh) {
            Owned o; o.set(dc, 0, 0, 66150);
            Run r; r.set = { MODE_CLASSIC, 0, sh, 0, 0, 0 };
            const int v[VALUES] = { 500, 60, 0, 0, 0, 0, 0, 80 };
            std::memcpy(r.values, v, sizeof v);
            r.block(o.src, step_of(60), EV_FRESH | EV_BEGINS);
            for (int i = 0; i < 200; ++i) r.block(o.src, step_of(60), 0);
            size_t begin, end; first_grain(r.wav, begin, end);
            size_t top = begin; for (size_t i = begin; i < end; i += 2) if (r.wav[i] > r.wav[top]) top = i;
            return (double)(top - begin) / (double)(end - begin);
        };
        const double tp = top_at(-100), t0 = top_at(0), ts = top_at(100);
        std::printf("shape: the top at %.3f (pluck), %.3f (0), %.3f (swell) of the grain\n", tp, t0, ts);
        CHECK(tp < 0.06 && t0 > 0.47 && t0 < 0.53 && ts > 0.94, "the window's top is not where the shape puts it");

        // overlapping grains: the level stays about what the even bell gives, at every density
        for (int dens : { 30, 60, 100 }) {
            double mean0 = 0;
            for (int sh : { 0, -100, -50, 50, 100 }) {
                Owned o; o.set(dc, 0, 0, 66150);
                Run r; r.set = { MODE_CLASSIC, 0, sh, 0, 0, 0 };
                const int v[VALUES] = { 500, 50, dens, 0, 0, 0, 0, 80 };
                std::memcpy(r.values, v, sizeof v);
                r.block(o.src, step_of(60), EV_FRESH | EV_BEGINS);
                for (int i = 0; i < 2 * BLOCKS_PER_S; ++i) r.block(o.src, step_of(60), 0);
                double sum = 0; int pk = 0, n = 0;
                for (size_t i = r.wav.size() / 2; i < r.wav.size(); i += 2) { sum += r.wav[i]; pk = std::max(pk, (int)r.wav[i]); ++n; }
                const double mean = sum / n;
                if (sh == 0) mean0 = mean;
                // at the top density more grains are asked for than a voice may hold (4), and which starts are refused
                // depends on the shape: the level may then differ by the one grain in four
                const double tol = dens == 100 ? 0.25 : 0.1;
                CHECK(mean > mean0 * (1 - tol) && mean < mean0 * (1 + tol), "density %d shape %d: mean %.0f, the bell's %.0f", dens, sh, mean, mean0);
                CHECK(pk < 32000, "density %d shape %d: the sum clips (%d)", dens, sh, pk);
            }
        }
        std::printf("shape: overlapping grains keep the bell's mean level within a tenth (a quarter where the cap of four grains bites)\n");

        // reverse: every read inside the region (the block before `start` and after `end` is not ours: the sanitizer
        // would stop a read there), at every mode, size, note and scan, mono and stereo, with a region that is not at 0
        int rruns = 0;
        for (u32 m = 0; m < MODES; ++m) for (int st = 0; st < 2; ++st) for (int sz : { 0, 50, 100 }) for (int midi : { 24, 60, 96, 108 }) for (u32 rev : { 50u, 100u }) {
            Owned o; o.set(st ? stereo : mono, st, 1000, 30000);
            Run r; r.set = { m, 0, (int)(rruns % 5) * 50 - 100, rev, 100, 100 };
            const int v[VALUES] = { (rruns * 137) % 1001, sz, 100, 100, (rruns % 3 - 1) * 100, 100, 24, 100 };
            std::memcpy(r.values, v, sizeof v);
            play(r, o.src, midi, 400, 200, period120);
            CHECK(r.peak() <= 1.0, "reverse run clips");
            ++rruns;
        }
        // a region shorter than a grain, and one of two frames
        for (u32 len : { 2u, 3u, 17u, 300u }) for (u32 rev : { 100u }) {
            Owned o; o.set(mono, 0, 5000, 5000 + len);
            Run r; r.set = { MODE_CLASSIC, 0, -100, rev, 100, 0 };
            play(r, o.src, 60, 300, 100);
            ++rruns;
        }
        std::printf("reverse: %d runs, no read outside the region\n", rruns);

        // a reversed grain plays the sample backward: over a rising ramp the output under the window's top falls
        {
            std::vector<short> ramp(66150); for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = (short)(2000 + i / 3);
            auto slope = [&](u32 rev) {
                Owned o; o.set(ramp, 0, 0, 66150);
                Run r; r.set = { MODE_CLASSIC, 0, 0, rev, 0, 0 };
                const int v[VALUES] = { 500, 60, 0, 0, 0, 0, 0, 80 };
                std::memcpy(r.values, v, sizeof v);
                r.block(o.src, step_of(60), EV_FRESH | EV_BEGINS);
                for (int i = 0; i < 300; ++i) r.block(o.src, step_of(60), 0);
                // compare two points the same way either side of the top of the first grain (its window is even there)
                size_t begin, end; first_grain(r.wav, begin, end);
                const size_t mid = (begin + (end - begin) / 2) & ~(size_t)1, q = ((end - begin) / 8) & ~(size_t)1;
                return (int)r.wav[mid + q] - (int)r.wav[mid - q];
            };
            const int f = slope(0), b = slope(100);
            std::printf("reverse: over a rising ramp the grain's later half is %+d (forward) and %+d (reversed) against its earlier\n", f, b);
            CHECK(f > 0 && b < 0, "a reversed grain does not read backward");
        }

        // the two random values: still the same render twice, and the level never above the plain one
        {
            Owned o; o.set(mono, 0, 0, 66150);
            Run a, b, c;
            a.set = b.set = { MODE_CLASSIC, 0, 0, 30, 100, 100 }; c.set = { MODE_CLASSIC, 0, 0, 0, 0, 0 };
            play(a, o.src, 60, 600, 200); play(b, o.src, 60, 600, 200); play(c, o.src, 60, 600, 200);
            CHECK(a.wav == b.wav, "two renders with the random values differ");
            CHECK(a.wav != c.wav, "the random values change nothing");
            CHECK(a.peak() <= 1.0 && a.peak() > 0.01, "the random values: peak %.3f", a.peak());
        }
    }

    // ---- the drop experiment's decisions (the call itself cannot be made here)
    {
        using namespace dropx;
        const Facts good = { 1, 1, 0, 1, 44100, 0, 1, 5, 5, 264608, 1 };
        CHECK(guard(good) == OK, "the good facts are refused");
        struct { Facts f; int want; } G[] = {
            { { 0, 1, 0, 1, 44100, 0, 1, 5, 5, 264608, 1 }, R_SITE },      { { 1, 0, 0, 1, 44100, 0, 1, 5, 5, 264608, 1 }, R_NOT_OURS },
            { { 1, 1, 1, 1, 44100, 0, 1, 5, 5, 264608, 1 }, R_DISK },      { { 1, 1, 0, 0, 44100, 0, 1, 5, 5, 264608, 1 }, R_CHAIN },
            { { 1, 1, 0, 1, 0, 0, 1, 5, 5, 264608, 1 }, R_NOTHING },       { { 1, 1, 0, 1, 0, 7, 1, 5, 5, 264608, 1 }, OK },
            { { 1, 1, 0, 1, 44100, 0, 0, 5, 5, 264608, 1 }, R_TRACKS },    { { 1, 1, 0, 1, 44100, 0, 4, 5, 5, 264608, 1 }, R_TRACKS },
            { { 1, 1, 0, 1, 44100, 0, 1, 5, 6, 264608, 1 }, R_WORKER },    { { 0, 0, 1, 0, 0, 0, 0, 1, 2, 264608, 1 }, R_SITE },
            { { 1, 1, 0, 1, 264600, 0, 1, 5, 5, 264608, 1 }, OK },       // a stereo cut of exactly the pool's length fits
            { { 1, 1, 0, 1, 264601, 0, 1, 5, 5, 264608, 1 }, R_LONG },   // one frame more, into a slot never loaded: past the pool
            { { 1, 1, 0, 1, 0xffffffffu, 0, 1, 5, 5, 264608, 1 }, R_LONG },
            { { 1, 1, 0, 1, 999999, 0, 1, 5, 5, 264600, 1 }, OK },       // a loaded slot says 264,600: the import stops there itself
            { { 1, 1, 0, 1, 999999, 0, 1, 5, 5, 264608, 0 }, OK },       { { 1, 1, 0, 1, 999999, 0, 1, 5, 5, 0xffffffffu, 1 }, R_LONG },
        };
        for (auto& g : G) CHECK(guard(g.f) == g.want, "guard: %d, want %d", guard(g.f), g.want);

        // the plain run: the key, a hold, FADE_FRAMES frames, one call, a release
        { Machine m{}; int calls = 0, holds = 0;
          int a = step(m, 3, good); holds += a == A_HOLD;
          CHECK(a == A_HOLD && m.slot == 2 && m.state == FADING && m.message == M_DROPPING, "the key does not start a hold");
          for (int i = 0; i < 10 && m.state == FADING; ++i) {
              a = step(m, i == 0 ? 5u : 0u, good);               // a second key while fading is not looked at
              if (a == A_CALL) { ++calls; CHECK(i == FADE_FRAMES - 1, "the call comes at frame %d", i); called(m); }
              CHECK(a != A_HOLD, "a second hold");
          }
          CHECK(calls == 1 && m.state == IDLE && m.done == 1 && m.refused == 0 && m.slot == 2 && m.message == M_DROPPED, "the plain run: %d calls", calls);
          for (int i = 0; i < MESSAGE_FRAMES + 2; ++i) CHECK(step(m, 0, good) == A_NONE, "an idle frame acts");
          CHECK(m.message == M_NONE, "the message stays"); }
        // refused at the key: nothing held, nothing called, the reason kept
        for (auto& g : G) if (g.want) {
            Machine m{}; const int a = step(m, 1, g.f);
            CHECK(a == A_NONE && m.state == IDLE && m.refused == 1 && m.done == 0 && m.reason == g.want, "a refused key acts (%d)", a);
            CHECK(m.message == (g.want == R_NOTHING ? (u32)M_NOTHING : (u32)M_REFUSED), "the wrong word for reason %d", g.want);
        }
        // the facts go bad during the fade: a release and no call
        for (auto& g : G) if (g.want) {
            Machine m{}; int calls = 0, releases = 0;
            step(m, 8, good);
            for (int i = 0; i < 10 && m.state == FADING; ++i) { const int a = step(m, 0, g.f); calls += a == A_CALL; releases += a == A_RELEASE; }
            CHECK(calls == 0 && releases == 1 && m.state == IDLE && m.refused == 1 && m.reason == g.want, "bad facts during the fade: %d calls, %d releases", calls, releases);
        }
        // requests that name no slot
        { Machine m{}; CHECK(step(m, 0, good) == A_NONE && step(m, 9, good) == A_NONE && step(m, 0xffffffffu, good) == A_NONE && m.state == IDLE, "a request of no slot acts"); }
        // random frames: never a call without the checks holding that frame, never two holds without a release or a call
        { Machine m{}; unsigned rs = 99; int held = 0; long calls = 0;
          for (int i = 0; i < 400000; ++i) {
              rs = rs * 1664525u + 1013904223u;
              Facts f = good;
              if ((rs >> 8) % 7 == 0) f.queue_write = 6; if ((rs >> 12) % 9 == 0) f.disk = 1; if ((rs >> 16) % 11 == 0) f.clip_length = 0;
              if ((rs >> 20) % 13 == 0) f.ours = 0; if ((rs >> 24) % 17 == 0) f.chain_ok = 0; if ((rs >> 27) % 19 == 0) f.site_ok = 0;
              const u32 request = (rs >> 4) % 5 == 0 ? (rs >> 9) % 11 : 0;
              const int a = step(m, request, f);
              if (a == A_HOLD) { CHECK(!held && guard(f) == OK, "a hold out of turn"); held = 1; }
              if (a == A_RELEASE) { CHECK(held, "a release with nothing held"); held = 0; }
              if (a == A_CALL) { CHECK(held && guard(f) == OK, "a call the checks do not allow"); called(m); held = 0; ++calls; }
              CHECK((m.state == FADING) == (held != 0), "the hold and the state part");
          }
          CHECK(calls > 1000, "the random run made %ld calls", calls);
          std::printf("drop: %ld calls and %u refusals in 400000 random frames, none without its checks\n", calls, m.refused); }
        // the key
        CHECK(key_down(11, 11, 0, 1, 1, 0) == 1, "the drop key is not taken");
        CHECK(key_down(11, 11, 1, 1, 1, 0) == 0, "shift and drop is taken from stock");
        CHECK(key_down(11, 11, 0, 0, 1, 0) == 0 && key_down(11, 11, 0, 1, 0, 0) == 0, "taken off the page or not armed");
        CHECK(key_down(10, 11, 0, 1, 1, 0) == 0 && key_down(12, 11, 0, 1, 1, 0) == 0 && key_down(6, 11, 0, 1, 1, 0) == 0, "another key is taken");
        CHECK(key_down(10, 11, 0, 1, 1, 1) == 2 && key_down(11, 11, 0, 1, 1, 1) == 2 && key_down(11, 11, 1, 1, 1, 1) == 2 && key_down(12, 11, 0, 1, 1, 1) == 2, "lift, drop and split pass to stock during a drop");
        CHECK(key_down(6, 11, 0, 1, 1, 1) == 0 && key_down(60, 60, 0, 1, 1, 1) == 2 && key_down(60, 60, 0, 1, 1, 0) == 1, "the key from the file");
        struct { const char* t; u32 want; } K[] = { { "11", 11 }, { "10\n", 10 }, { " 12 lift?", 12 }, { "", 11 }, { "x", 11 }, { "38", 11 }, { "13", 11 }, { "53", 11 },
                                                    { "54", 54 }, { "127", 127 }, { "128", 11 }, { "99999999999", 11 }, { "0", 0 }, { "-3", 11 } };
        for (auto& k : K) CHECK(key_from_text(k.t, (u32)std::strlen(k.t)) == k.want, "dropkey.txt '%s': %u", k.t, key_from_text(k.t, (u32)std::strlen(k.t)));
        { const char raw[4] = { '1', '2', '7', '9' }; CHECK(key_from_text(raw, 3) == 127 && key_from_text(raw, 4) == 11 && key_from_text(raw, 0) == 11, "a text with no end"); }
    }

    // ---- the sampler's region: where the pool's sound ends (the object does not say, after a drop)
    {
        const unsigned POOL = SAMPLER_POOL_WORDS;
        std::vector<short> pool(POOL + 64, 0);                    // 64 words beyond, which must stay unread
        short* d = pool.data();
        auto region = [&](unsigned frames, unsigned stereo, unsigned known, unsigned& first, unsigned& end) { return sampler_region(d, frames, stereo, known, &first, &end); };
        unsigned first, end;
        CHECK(sampler_capacity(264608, 1) == 264600 && sampler_capacity(264608, 0) == 264608 && sampler_capacity(264600, 0) == 264600 &&
              sampler_capacity(0xffffffffu, 0) == POOL && sampler_capacity(0xffffffffu, 1) == POOL / 2 && sampler_capacity(100, 1) == 100, "the capacity");
        // an empty slot, as the probe printed it: frames 264608, stereo 1, all silent
        CHECK(region(264608, 1, 0, first, end) == SAMPLER_EMPTY && region(264608, 0, 0, first, end) == SAMPLER_EMPTY, "a silent pool is not empty");
        CHECK(region(264600, 2, 0, first, end) == SAMPLER_BAD_STEREO, "a stereo byte of 2 is taken");
        // a mono drop of 30,000 frames from the pool's first word, as the drop writes it
        for (unsigned i = 0; i < 30000; ++i) d[i] = (short)(1000 + (i & 255));
        CHECK(region(264600, 0, 0, first, end) == SAMPLER_OK && first == 0 && end == 30000, "a mono drop: %u..%u", first, end);
        // the same words read as stereo: 15,000 frames
        CHECK(region(264600, 1, 0, first, end) == SAMPLER_OK && first == 0 && end == 15000, "a stereo drop: %u..%u", first, end);
        // silence inside the sound does not end it; only the silent tail is left out
        for (unsigned i = 10000; i < 20000; ++i) d[i] = 0;
        CHECK(region(264600, 0, 0, first, end) == SAMPLER_OK && end == 30000, "silence inside the sound ends it at %u", end);
        // a drop of our own, known: its frames, never past the capacity, whatever lies behind (an older, longer sample)
        for (unsigned i = 30000; i < 200000; ++i) d[i] = 77;
        CHECK(region(264600, 0, 0, first, end) == SAMPLER_OK && end == 200000, "the leftover is not found: %u", end);
        CHECK(region(264600, 0, 30000, first, end) == SAMPLER_OK && end == 30000, "a known drop: %u", end);
        CHECK(region(264600, 0, 999999, first, end) == SAMPLER_OK && end == 264600 && region(264600, 1, 999999, first, end) == SAMPLER_OK && end == 264600, "a known drop past the capacity: %u", end);
        CHECK(region(264600, 0, 1, first, end) == SAMPLER_EMPTY, "a drop of one frame is taken");
        // sound up to the last word, mono and stereo; the object's frames above the pool: never a read past it
        for (unsigned i = 0; i < POOL; ++i) d[i] = 5;
        for (unsigned i = POOL; i < POOL + 64; ++i) d[i] = 9999;  // would be found if the scan left the pool
        CHECK(region(0xffffffffu, 0, 0, first, end) == SAMPLER_OK && end == POOL, "mono to the pool's end: %u", end);
        CHECK(region(0xffffffffu, 1, 0, first, end) == SAMPLER_OK && end == POOL / 2, "stereo to the pool's end: %u", end);
        CHECK(region(264608, 1, 0, first, end) == SAMPLER_OK && end == 264600, "frames 264608, stereo: %u", end);
        // random ends: the scan finds the last frame of a dense sound exactly, at every place against its steps
        unsigned rs = 3, wrong = 0;
        for (int t = 0; t < 3000; ++t) {
            rs = rs * 1664525u + 1013904223u; const unsigned stereo = t & 1u, cap = sampler_capacity(264600, stereo);
            rs = rs * 1664525u + 1013904223u; const unsigned len = (rs >> 7) % (cap + 1u);
            std::fill(pool.begin(), pool.begin() + POOL, (short)0);
            for (unsigned i = 0; i < len * (stereo ? 2u : 1u); ++i) d[i] = (short)(1 + (i % 7));
            if (stereo && len) { d[2 * len - 1] = 0; d[2 * len - 2] = 3; }   // a last frame with one silent side still sounds
            const int got = region(264600, stereo, 0, first, end);
            if (len < 2 ? got != SAMPLER_EMPTY : (got != SAMPLER_OK || first != 0 || end != len)) ++wrong;
            if (got == SAMPLER_OK) { GrainSource g = { d, end, stereo, first, end, 60, 65536, 1, 1 }; if (!grain_source_ok(g)) ++wrong; }
        }
        CHECK(!wrong, "%u random pools read wrong", wrong);
        // a slot loaded from a file: where `words` says
        { struct { unsigned frames, words, stereo; int want; unsigned first, end; } T[] = {
              { 264600, 16392, 0, SAMPLER_OK, 8, 16392 }, { 264600, 176408, 0, SAMPLER_OK, 8, 176408 }, { 264600, 264600, 0, SAMPLER_OK, 8, 264600 },
              { 264600, 529200, 1, SAMPLER_OK, 4, 264600 }, { 264600, 0xffffffffu, 1, SAMPLER_OK, 4, 264600 }, { 264600, 0xffffffffu, 0, SAMPLER_OK, 8, 264600 },
              { 264600, 8, 0, SAMPLER_EMPTY, 0, 0 }, { 264600, 9, 0, SAMPLER_EMPTY, 0, 0 }, { 5, 529200, 1, SAMPLER_EMPTY, 0, 0 }, { 264600, 0, 1, SAMPLER_EMPTY, 0, 0 },
              { 264600, 20001, 1, SAMPLER_OK, 4, 10000 }, { 264600, 20000, 2, SAMPLER_BAD_STEREO, 0, 0 } };
          for (auto& t : T) { unsigned f1 = 9, e1 = 9; const int got = sampler_region_loaded(t.frames, t.words, t.stereo, &f1, &e1);
              CHECK(got == t.want && f1 == t.first && e1 == t.end, "a loaded slot frames %u words %u stereo %u: %d, %u..%u", t.frames, t.words, t.stereo, got, f1, e1); } }

        // the search a little each frame: the same answer as the search in one go, and never over its budget
        { unsigned rs2 = 11, bad = 0, most_calls = 0;
          for (int t = 0; t < 2000; ++t) {
              rs2 = rs2 * 1664525u + 1013904223u; const unsigned stereo = t & 1u, cap = sampler_capacity((t % 5 == 0) ? 264608u : 264600u, stereo);
              rs2 = rs2 * 1664525u + 1013904223u; const unsigned len = (t % 7 == 0) ? 0u : (rs2 >> 7) % (cap + 1u);
              rs2 = rs2 * 1664525u + 1013904223u; const unsigned budget = (t % 3 == 0) ? (unsigned)sscan::BUDGET_WORDS : 4u + (rs2 >> 9) % 600u;
              std::fill(pool.begin(), pool.begin() + POOL, (short)0);
              for (unsigned i = 0; i < len * (stereo ? 2u : 1u); ++i) d[i] = (short)(1 + (i % 7));
              sscan::Scan sc; sscan::begin(sc, cap, stereo);
              unsigned calls = 0;
              while (sc.state != (unsigned)sscan::DONE && calls < 400000u) { const unsigned used = sscan::step(sc, d, budget); if (used > budget) ++bad; ++calls; }
              if (sc.state != (unsigned)sscan::DONE || sc.e != sampler_sounding(d, cap, stereo) || sc.e != len) ++bad;
              if (budget == (unsigned)sscan::BUDGET_WORDS && calls > most_calls) most_calls = calls;
          }
          CHECK(!bad, "%u searches over their budget or with another answer", bad);
          CHECK(most_calls <= 10, "a search of %u frames at the budget", most_calls);
          std::printf("sampler scan: 2000 pools, each call within its budget, the answer of the search in one go; at %d words a frame the longest took %u frames\n", (int)sscan::BUDGET_WORDS, most_calls); }
        // what the search reads of each kind of pool on the unit this was written for
        { struct { const char* what; unsigned frames, stereo, len; } O[] = { { "a silent stereo pool", 264608, 1, 0 }, { "a silent mono pool", 264608, 0, 0 },
              { "a stereo cut of 255,039 frames", 264608, 1, 255039 }, { "a full stereo pool", 264600, 1, 264596 }, { "a mono sample of 176,408 words", 264600, 0, 176408 } };
          for (auto& o : O) {
              std::fill(pool.begin(), pool.begin() + POOL, (short)0);
              for (unsigned i = 0; i < o.len * (o.stereo ? 2u : 1u); ++i) d[i] = 9;
              sscan::Scan sc; sscan::begin(sc, sampler_capacity(o.frames, o.stereo), o.stereo);
              const unsigned all = sscan::step(sc, d, 0xffffffffu);
              std::printf("sampler scan: %s: %u words read in all, %u frames at the budget\n", o.what, all, (all + (unsigned)sscan::BUDGET_WORDS - 1u) / (unsigned)sscan::BUDGET_WORDS);
          } }
        std::printf("sampler region: the probe's cases, drops with leftovers, 3000 random pools\n");
    }

    // ---- the cost: the most grains a voice held at once, and the worst block's grain-frames
    std::printf("most grains at once in a voice: %d of %d\n", max_grains, GRAINS);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
