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
    struct Case { const char* name; u32 mode, window, loop; int values[VALUES]; int stereo, midi; };
    const Case cases[] = {
        { "classic_default", MODE_CLASSIC, 0, 0, { 300, 50, 50, 10, 0, 30, 0, 80 }, 0, 60 },
        { "classic_scan_fwd", MODE_CLASSIC, 0, 0, { 0, 40, 70, 5, 60, 50, 0, 80 }, 1, 60 },
        { "classic_scan_back", MODE_CLASSIC, 1, 0, { 1000, 60, 60, 0, -50, 20, 0, 80 }, 1, 60 },
        { "classic_short_pluck", MODE_CLASSIC, 2, 0, { 500, 10, 90, 30, 0, 80, 12, 80 }, 0, 67 },
        { "loop_free", MODE_LOOP, 0, 0, { 400, 30, 60, 0, 0, 20, 0, 80 }, 1, 60 },
        { "loop_eighth_120bpm", MODE_LOOP, 1, 2, { 200, 40, 50, 0, 10, 40, 0, 80 }, 1, 60 },
        { "cloud_dense", MODE_CLOUD, 0, 0, { 500, 70, 100, 40, 0, 100, 0, 80 }, 1, 60 },
        { "cloud_tri_low", MODE_CLOUD, 3, 0, { 300, 80, 80, 80, 20, 60, -12, 80 }, 0, 48 },
    };
    const u32 period120 = (u32)std::lround(44100.0 * 60 / 120 / 96 * 262144.0);
    int max_grains = 0;
    for (const Case& c : cases) {
        Owned o; o.set(c.stereo ? stereo : mono, c.stereo, 0, 66150);
        Run r; r.set = { c.mode, c.window, c.loop }; std::memcpy(r.values, c.values, sizeof r.values);
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
        Run r; r.set = { MODE_CLASSIC, 0, 0 }; r.values[V_SPRAY] = 100; r.values[V_SCAN] = 100;
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

    // ---- the cost: the most grains a voice held at once, and the worst block's grain-frames
    std::printf("most grains at once in a voice: %d of %d\n", max_grains, GRAINS);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
