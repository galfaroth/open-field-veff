// Host test of veffgraindelay's sound and page, on a computer (not a unit). Built by run_tests.sh with
// -DOP1F_HOST: the Blackfin builtins are replaced by the fallbacks in veffgraindelay.cpp, the base's helpers by the
// stand-ins below. It checks, and stops with a line naming what failed:
//   - the state's size is the recipe's state_bytes;
//   - mix 0 leaves the sound bit for bit;
//   - pitch 0, spray 0, random 0, spread 0, feedback 0 is a plain delay of DELAY frames (within the line's 16 bits);
//   - tempo sync: 1/4 at 120 bpm is 22050 frames (as the tempo's period says), 1/8 half of it, an impulse comes out there;
//   - every value at its far end, feedback 95, full-scale square in for 20 s: the output stays bounded and, within 60 s
//     of the input stopping, dies to exact silence (no lingering low bits, no latched DC); and the longest tail (2 s
//     delay, pitch 0, no random, feedback 95) dies to exact silence too, in the time it is printed;
//   - silence in from a fresh start gives silence out;
//   - the same input and values give the same output, bit for bit (the random is seeded);
//   - the page paints without reading out of bounds.
// It also writes WAV files of test input through several settings, into the folder given as the first argument, and
// the page at 2x as a PPM there (page.ppm). Run it under the address and undefined-behaviour sanitizers.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "../veffgraindelay.cpp"

// ---------------------------------------------------------------- the base's helpers, stood in
extern "C" const Op1fCard op1f_card_veffgraindelay = {};
static int host_values[4] = { 11585, 15, 45, 35 };
extern "C" int op1f_param_heard(const Op1fCard*, unsigned, unsigned, unsigned index, int* out) { *out = host_values[index & 3]; return 0; }
extern "C" int op1f_param_set(const Op1fCard*, unsigned, unsigned, unsigned index, int value) { host_values[index & 3] = value; return 0; }
extern "C" int op1f_value_get(const Op1fCard*, unsigned, unsigned, const char*, char*, unsigned) { return 0; }
extern "C" int op1f_value_set(const Op1fCard*, unsigned, unsigned, const char*, const char*) { return 0; }
extern "C" void* op1f_picture(const Op1fCard*, const char*) { return 0; }
extern "C" int op1f_page_stand_in(const Op1fCard*, unsigned, void*) { return 0; }
extern "C" unsigned op1f_riding(const Op1fCard*, unsigned) { return 1u; }

// the canvas: drawn at 4x, kept at 2x (each kept pixel the mean of four)
enum { SS = 4, CW = 480 * SS, CH = 224 * SS };
static std::vector<uint8_t> canvas_rgb;
static void plot(int x, int y, unsigned c) {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return;
    uint8_t* p = &canvas_rgb[(size_t)(y * CW + x) * 3];
    p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); p[2] = (uint8_t)((c & 31) * 255 / 31);
}
extern "C" void op1f_paint_box(void*, int x, int y, int w, int h, unsigned c) {
    for (int j = y * SS; j < (y + h) * SS; ++j) for (int i = x * SS; i < (x + w) * SS; ++i) plot(i, j, c);
}
extern "C" void op1f_paint_line(void*, int x0, int y0, int x1, int y1, int width, unsigned c) {
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

// ---------------------------------------------------------------- running the effect
static const unsigned PERIOD_120 = 60214886u;                         // 229.7 * 2^18: 120 bpm
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

struct Setting { const char* name; int v[4]; int x[EXTRAS]; };

struct Runner {
    void* mem;
    int values[4];
    bool first = true;
    unsigned period = PERIOD_120;
    explicit Runner(const Setting& s) {
        mem = std::calloc(1, STATE_BYTES);
        std::memcpy(values, s.v, sizeof values);
        for (int k = 0; k < EXTRAS; ++k) extra[0][k] = s.x[k];
        extra_ready |= 1u;
        veffgraindelay_reset(0, mem);
    }
    ~Runner() { std::free(mem); }
    // in and out: interleaved stereo Q1.31, a whole number of blocks
    void run(std::vector<int>& l, std::vector<int>& r) {
        for (size_t at = 0; at + BLOCK <= l.size(); at += BLOCK) {
            Op1fEffectBlock b = {};
            b.bytes = 44; b.place = 0; b.frames = BLOCK; b.left = &l[at]; b.right = &r[at];
            b.values = values; b.on = 1; b.fresh = first ? 1 : 0; b.period = period; b.playing = 1; b.state = mem;
            first = false;
            veffgraindelay_process(&b);
        }
    }
};

static const int Q = 0x7fffffff;
static int q31(double x) { x = x > 1 ? 1 : x < -1 ? -1 : x; return (int)std::lround(x * 2147483647.0); }

static uint32_t lcg = 12345u;
static double noise() { lcg = lcg * 1664525u + 1013904223u; return ((int32_t)lcg) / 2147483648.0; }

// test inputs, mono into both sides (the phrase is panned a little)
static void make_impulses(std::vector<int>& l, std::vector<int>& r, size_t n) {
    l.assign(n, 0); r.assign(n, 0);
    for (size_t i = 4410; i < n / 2; i += 44100) { l[i] = q31(0.8); r[i] = q31(0.8); }
}
static void make_sine(std::vector<int>& l, std::vector<int>& r, size_t n, double hz, double amp, size_t on) {
    l.assign(n, 0); r.assign(n, 0);
    for (size_t i = 0; i < on && i < n; ++i) { const double e = i < 441 ? i / 441.0 : on - i < 441 ? (on - i) / 441.0 : 1; l[i] = r[i] = q31(amp * e * std::sin(2 * M_PI * hz * i / 44100.0)); }
}
static void make_bursts(std::vector<int>& l, std::vector<int>& r, size_t n) {
    l.assign(n, 0); r.assign(n, 0);
    for (size_t b = 0; b < 4; ++b) for (size_t i = 0; i < 4410; ++i) {
        const size_t at = 22050 + b * 33075 + i; if (at >= n) break;
        const double e = std::exp(-(double)i / 1200.0);
        l[at] = q31(0.5 * e * noise()); r[at] = q31(0.5 * e * noise());
    }
}
static void make_phrase(std::vector<int>& l, std::vector<int>& r, size_t n) {   // plucked notes, 120 bpm eighths
    l.assign(n, 0); r.assign(n, 0);
    static const int NOTES_[] = { 60, 64, 67, 72, 71, 67, 64, 62, 60, -1, 67, -1, 72, 76, 74, 72 };
    for (int k = 0; k < 16; ++k) {
        if (NOTES_[k] < 0) continue;
        const double hz = 440.0 * std::pow(2.0, (NOTES_[k] - 69) / 12.0), pan = (k & 1) ? 0.35 : -0.35;
        const size_t at = (size_t)k * 11025;
        for (size_t i = 0; i < 16000 && at + i < n; ++i) {
            const double t = i / 44100.0, e = std::exp(-t * 6.0) * (i < 44 ? i / 44.0 : 1.0);
            const double s = 0.35 * e * (std::sin(2 * M_PI * hz * t) + 0.3 * std::sin(4 * M_PI * hz * t) + 0.12 * std::sin(6 * M_PI * hz * t));
            l[at + i] += q31(s * (1 - pan) * 0.7); r[at + i] += q31(s * (1 + pan) * 0.7);
        }
    }
}

static void write_wav(const std::string& path, const std::vector<int>& l, const std::vector<int>& r) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
    const uint32_t frames = (uint32_t)l.size(), data = frames * 4, riff = 36 + data;
    const uint16_t fmt[] = { 1, 2 };
    const uint32_t rate = 44100, bps = 44100 * 4; const uint16_t align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t sixteen = 16; std::fwrite(&sixteen, 4, 1, f); std::fwrite(fmt, 2, 2, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&bps, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) { const int16_t s[2] = { (int16_t)(l[i] >> 16), (int16_t)(r[i] >> 16) }; std::fwrite(s, 2, 2, f); }
    std::fclose(f);
}

static uint64_t hash(const std::vector<int>& a, const std::vector<int>& b) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < a.size(); ++i) { h = (h ^ (uint32_t)a[i]) * 1099511628211ull; h = (h ^ (uint32_t)b[i]) * 1099511628211ull; }
    return h;
}

static size_t round_blocks(size_t n) { return (n + BLOCK - 1) / BLOCK * BLOCK; }

// the settings rendered (values: pitch, spray ms, feedback, mix; extras: delay step, freq step, random, spread, reverse)
enum { MS100 = NOTES + 36, MS250 = NOTES + 46, MS10 = NOTES + 10 };
static const Setting START = { "start", { 11585, 15, 45, 35 }, { 4, 34, 0, 40, 0 } };
static const Setting SETTINGS[] = {
    START,
    { "plain-delay", { 0, 0, 30, 50 }, { MS250, 34, 0, 0, 0 } },
    { "fifth-up-wide", { 7347, 40, 60, 50 }, { 3, 40, 10, 100, 0 } },          // +7 st (7347 squared is ~700 cents)
    { "octave-down-reverse", { -11585, 30, 55, 60 }, { 4, 30, 0, 60, 1 } },
    { "spray-cloud", { 0, 400, 70, 70 }, { MS100, 22, 35, 100, 0 } },
    { "random-glitch", { 2000, 120, 50, 60 }, { 5, 55, 100, 80, 0 } },
    { "short-tight", { 4000, 5, 80, 50 }, { MS10, 60, 0, 30, 0 } },
};

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    std::printf("state %zu bytes (recipe 524928)\n", sizeof(State));
    CHECK(STATE_BYTES == 524928, "state_bytes %d", (int)STATE_BYTES);

    // ---- mix 0: bit for bit
    {
        Setting s = START; s.v[V_MIX] = 0;
        Runner rn(s);
        std::vector<int> l, r; make_phrase(l, r, round_blocks(88200));
        const std::vector<int> l0 = l, r0 = r;
        rn.run(l, r);
        CHECK(l == l0 && r == r0, "mix 0 changed the sound");
    }

    // ---- plain delay: pitch 0, spray 0, random 0, spread 0, feedback 0, mix 100
    {
        Setting s = { "plain", { 0, 0, 0, 100 }, { MS100, 34, 0, 0, 0 } };
        Runner rn(s);
        const u32 d = delay_frames(MS100, PERIOD_120);
        CHECK(d == 4410u, "100 ms is %u frames", d);
        std::vector<int> l(round_blocks(5 * 44100)), r(l.size());
        for (size_t i = 0; i < l.size(); ++i) { l[i] = q31(0.4 * noise()); r[i] = q31(0.4 * std::sin(2 * M_PI * 330 * i / 44100.0)); }
        const std::vector<int> l0 = l, r0 = r;
        rn.run(l, r);
        double worst = 0;
        for (size_t i = 44100; i < l.size(); ++i) {
            const double el = std::fabs((double)l[i] - (double)l0[i - d]) / 2147483648.0;
            const double er = std::fabs((double)r[i] - (double)r0[i - d]) / 2147483648.0;
            if (el > worst) worst = el;
            if (er > worst) worst = er;
        }
        std::printf("plain delay: worst error against the input %u frames back: %.2e of full scale (%.1f dB)\n", d, worst, 20 * std::log10(worst + 1e-12));
        CHECK(worst < 2e-4, "plain delay error %.3g", worst);
    }

    // ---- tempo sync: 1/4 and 1/8 at 120 bpm; an impulse comes out DELAY later
    {
        const u32 q = delay_frames(3, PERIOD_120), e = delay_frames(4, PERIOD_120), b = delay_frames(2, PERIOD_120);
        const u32 slow = delay_frames(3, PERIOD_120 * 2u);              // 60 bpm
        std::printf("tempo sync at 120 bpm: 1/2 %u, 1/4 %u, 1/8 %u frames; 1/4 at 60 bpm %u\n", b, q, e, slow);
        CHECK(q >= 22048 && q <= 22052, "1/4 at 120 bpm is %u, not ~22050", q);
        CHECK(e == q / 2 || e == (q + 1) / 2, "1/8 %u not half of 1/4 %u", e, q);
        CHECK(slow >= 44096 && slow <= 44104, "1/4 at 60 bpm is %u", slow);
        Setting s = { "sync", { 0, 0, 0, 100 }, { 3, 34, 0, 0, 0 } };
        Runner rn(s);
        std::vector<int> l(round_blocks(3 * 44100), 0), r(l.size(), 0);
        const size_t at = 44100 + 17;
        l[at] = r[at] = q31(0.5);
        rn.run(l, r);
        size_t best = 0; int peak = 0;
        for (size_t i = 0; i < l.size(); ++i) if (std::abs(l[i]) > peak) { peak = std::abs(l[i]); best = i; }
        std::printf("tempo sync: impulse at %zu came out at %zu (%zu frames later, %.3f of its height)\n", at, best, best - at, peak / (double)q31(0.5));
        CHECK(best - at == q, "impulse delayed %zu, expected %u", best - at, q);
    }

    // ---- silence in, from a fresh start: silence out
    for (const Setting& s : SETTINGS) {
        Runner rn(s);
        std::vector<int> l(round_blocks(3 * 44100), 0), r(l.size(), 0);
        rn.run(l, r);
        bool all = true;
        for (size_t i = 0; i < l.size(); ++i) if (l[i] || r[i]) { all = false; break; }
        CHECK(all, "%s: silence in gave sound out", s.name);
    }

    // ---- the far ends: full-scale square, feedback 95, for 20 s, then 60 s of silence
    {
        static const int PITCHES[] = { -16383, 0, 16383 };
        static const int SPRAYS[] = { 0, 500 };
        static const int DELAYS[] = { 0, NOTES, NOTES + 1, 0 + NOTES + MS_STEPS - 1, 7 };
        static const int FREQS_[] = { 0, FREQS - 1 };
        int runs = 0, silent = 0;
        double worst_tail = 0, loud = 0;
        for (int pi : PITCHES) for (int sp : SPRAYS) for (int de : DELAYS) for (int fq : FREQS_) for (int rv = 0; rv < 2; ++rv) {
            Setting s = { "far", { pi, sp, 95, 100 }, { de, fq, 100, 100, rv } };
            if (de == 0) s.x[E_DELAY] = 0;                              // 2/1: past the line's most, held
            Runner rn(s);
            const size_t on = round_blocks(20 * 44100), n = round_blocks(80 * 44100);
            std::vector<int> l(n, 0), r(n, 0);
            for (size_t i = 0; i < on; ++i) { const int v = ((i / 50) & 1) ? Q : -Q; l[i] = v; r[i] = (i / 73) & 1 ? Q : -Q; }
            rn.run(l, r);
            // the wet's loudness while the input plays, and what is left at the end
            double peak_on = 0;
            for (size_t i = on / 2; i < on; ++i) peak_on = std::fmax(peak_on, std::fabs((double)l[i]) / 2147483648.0);
            loud = std::fmax(loud, peak_on);
            double tail = 0, after = 0;
            for (size_t i = n - 5 * 44100; i < n; ++i) tail = std::fmax(tail, std::fmax(std::fabs((double)l[i]), std::fabs((double)r[i])));
            for (size_t i = on; i < on + 5 * 44100; ++i) after = std::fmax(after, std::fmax(std::fabs((double)l[i]), std::fabs((double)r[i])));
            worst_tail = std::fmax(worst_tail, tail);
            if (tail == 0) ++silent;
            // a tail longer than the run (a 2 s line at 95 %) must at least be 40 dB down and falling
            CHECK(tail == 0 || tail < after / 100, "far ends: pitch %d spray %d delay %d freq %d reverse %d: the tail does not die (%.0f then %.0f)", pi, sp, de, fq, rv, after, tail);
            ++runs;
        }
        std::printf("far ends: %d runs of 80 s, full-scale square for 20 s at feedback 95: peak while playing %.3f; "
                    "%d of them exactly silent in the last 5 s, the others' worst peak there %.1f dBFS\n",
                    runs, loud, silent, 20 * std::log10(worst_tail / 2147483648.0));
    }

    // ---- the longest tails: a 2 s line (2/1 held), feedback 95: plain, and +24 st reversed with every random
    {
        static const Setting TAILS[2] = { { "tail plain", { 0, 0, 95, 100 }, { 0, 34, 0, 0, 0 } },
                                          { "tail +24 reverse", { 16383, 500, 95, 100 }, { 0, 0, 100, 100, 1 } } };
        for (const Setting& s : TAILS) {
            Runner rn(s);
            const size_t on = round_blocks(5 * 44100), n = round_blocks(900 * 44100);
            std::vector<int> l(n, 0), r(n, 0);
            for (size_t i = 0; i < on; ++i) { l[i] = ((i / 50) & 1) ? Q : -Q; r[i] = q31(0.9 * noise()); }
            rn.run(l, r);
            size_t last = 0;
            for (size_t i = 0; i < n; ++i) if (l[i] || r[i]) last = i;
            std::printf("%s: the last sound %.1f s after the input stopped, exact silence after\n", s.name, (double)(last - on) / 44100.0);
            CHECK(last + 10 * 44100 < n, "%s is still sounding at %.1f s", s.name, (double)last / 44100.0);
        }
    }

    // ---- a long run at the most feedback with pitch up and noise in: bounded, no run-away
    {
        Setting s = { "long", { 16383, 0, 95, 100 }, { MS100, 34, 0, 0, 0 } };
        Runner rn(s);
        const size_t n = round_blocks(120 * 44100);
        std::vector<int> l(n), r(n);
        for (size_t i = 0; i < n; ++i) { l[i] = q31(0.5 * noise()); r[i] = q31(0.5 * noise()); }
        rn.run(l, r);
        double e1 = 0, e2 = 0;
        for (size_t i = 10 * 44100; i < 20 * 44100; ++i) e1 += std::pow(l[i] / 2147483648.0, 2);
        for (size_t i = n - 10 * 44100; i < n; ++i) e2 += std::pow(l[i] / 2147483648.0, 2);
        e1 = std::sqrt(e1 / (10 * 44100)); e2 = std::sqrt(e2 / (10 * 44100));
        std::printf("120 s of noise at feedback 95, +24 st: RMS at 10-20 s %.3f, at the last 10 s %.3f\n", e1, e2);
        CHECK(e2 < 1.0 && e2 < e1 * 1.5, "the loop grows: %.3f then %.3f", e1, e2);
    }

    // ---- no clicks at the grains' edges: a quiet sine through scattered, pitched grains moves smoothly. A grain that
    //      began or ended with a step would jump by up to the sine's height; a smooth window keeps every step near
    //      what the pitched sine's own slope gives (2 pi f r / 44100 of its height for each of two grains)
    {
        Setting s = { "edges", { 6000, 300, 0, 100 }, { MS100, 50, 50, 0, 0 } };    // ~+3.2 st, 300 ms spray, random 50
        Runner rn(s);
        std::vector<int> l, r; make_sine(l, r, round_blocks(6 * 44100), 220.0, 0.5, 6 * 44100);
        rn.run(l, r);
        double step = 0;
        for (size_t i = 44100 + 1; i < l.size() - 44100; ++i) step = std::fmax(step, std::fabs((double)l[i] - (double)l[i - 1]) / 2147483648.0);
        const double slope = 2 * M_PI * 220 * 1.6 / 44100 * 0.5;          // the fastest grain's slope (random up to +9.2 st)
        std::printf("grain edges: the largest step of the wet %.4f; a pitched sine's own slope %.4f a frame\n", step, slope);
        CHECK(step < 2.5 * slope, "a step of %.4f: a click", step);
    }

    // ---- the freq turned between 1 Hz and 150 Hz at once: grains overlap (up to three), the sum stays held
    {
        Setting s = { "jump", { 16383, 500, 95, 100 }, { 0, 0, 100, 100, 0 } };
        Runner rn(s);
        const size_t part = 44100 / 2 / BLOCK * BLOCK;
        std::vector<int> l(round_blocks(3 * 44100)), r(l.size());
        for (size_t i = 0; i < l.size(); ++i) { l[i] = ((i / 50) & 1) ? Q : -Q; r[i] = q31(noise()); }
        rn.run(l, r);
        int most = 0;
        for (int k = 0; k < 8; ++k) {
            extra[0][E_FREQ] = k & 1 ? 0 : FREQS - 1;
            std::vector<int> c(part), d(part);
            for (size_t i = 0; i < part; ++i) { c[i] = ((i / 50) & 1) ? Q : -Q; d[i] = q31(noise()); }
            for (size_t i = 0; i < part; i += BLOCK) {
                std::vector<int> e(c.begin() + i, c.begin() + i + BLOCK), g(d.begin() + i, d.begin() + i + BLOCK);
                rn.run(e, g);
                const State& st = *(const State*)rn.mem;
                int live = 0; for (int q = 0; q < GRAINS; ++q) live += st.g[q].live ? 1 : 0;
                most = live > most ? live : most;
            }
        }
        std::printf("freq jumps between 1 Hz and 150 Hz: up to %d grains live at a block's end\n", most);
        CHECK(most <= GRAINS, "more grains than slots");
    }

    // ---- the same twice: bit for bit
    {
        uint64_t h[2];
        for (int k = 0; k < 2; ++k) {
            Runner rn(SETTINGS[5]);
            std::vector<int> l, r; make_phrase(l, r, round_blocks(6 * 44100));
            rn.run(l, r);
            h[k] = hash(l, r);
        }
        std::printf("determinism: %016llx %016llx\n", (unsigned long long)h[0], (unsigned long long)h[1]);
        CHECK(h[0] == h[1], "two runs differ");
    }

    // ---- renders
    struct Input { const char* name; void (*make)(std::vector<int>&, std::vector<int>&, size_t); };
    auto sine = [](std::vector<int>& l, std::vector<int>& r, size_t n) { make_sine(l, r, n, 440.0, 0.3162, 2 * 44100); };
    const Input INPUTS[] = { { "impulses", make_impulses }, { "sine", +sine }, { "bursts", make_bursts }, { "phrase", make_phrase } };
    int wavs = 0;
    for (const Setting& s : SETTINGS) for (const Input& in : INPUTS) {
        Runner rn(s);
        std::vector<int> l, r; in.make(l, r, round_blocks(8 * 44100));
        rn.run(l, r);
        double pk = 0;
        for (size_t i = 0; i < l.size(); ++i) pk = std::fmax(pk, std::fmax(std::fabs((double)l[i]), std::fabs((double)r[i])) / 2147483648.0);
        write_wav(out + "/" + s.name + "-" + in.name + ".wav", l, r);
        if (in.make == make_phrase) std::printf("render %-20s phrase: peak %.3f\n", s.name, pk);
        ++wavs;
    }
    { std::vector<int> l, r; make_phrase(l, r, round_blocks(8 * 44100)); write_wav(out + "/dry-phrase.wav", l, r); }
    std::printf("wrote %d WAV files (+ dry-phrase.wav) to %s\n", wavs, out.c_str());

    // ---- the page: the start values, the phrase playing, shown at place 0, then the shift set
    for (int shift = 0; shift < 2; ++shift) {
        canvas_rgb.assign((size_t)CW * CH * 3, 0);
        Runner rn(START);
        std::vector<int> l, r; make_phrase(l, r, round_blocks(2 * 44100 + 64 * 37));
        rn.run(l, r);
        veffgraindelay_shown(0, OP1F_SHOWN_UP);
        Op1fFrame fr = {}; fr.bytes = sizeof fr; fr.page = &op1f_card_veffgraindelay; fr.canvas = &fr; fr.shift = (unsigned)shift;
        for (int k = 0; k < 6; ++k) veffgraindelay_frame(&fr);             // the envelope eases in
        canvas_rgb.assign((size_t)CW * CH * 3, 0);
        veffgraindelay_frame(&fr);
        const std::string path = out + (shift ? "/page-shift.ppm" : "/page.ppm");
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", CW / 2, CH / 2);
            for (int y = 0; y < CH; y += 2) for (int x = 0; x < CW; x += 2) for (int c = 0; c < 3; ++c) {
                const int sum = canvas_rgb[((size_t)y * CW + x) * 3 + c] + canvas_rgb[((size_t)y * CW + x + 1) * 3 + c]
                              + canvas_rgb[((size_t)(y + 1) * CW + x) * 3 + c] + canvas_rgb[((size_t)(y + 1) * CW + x + 1) * 3 + c];
                std::fputc(sum / 4, f);
            }
            std::fclose(f);
        }
    }

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
