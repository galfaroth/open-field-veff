// veffdrumprobe: a read-only probe of the drum sampler, for OS 1.7.3 (1.7.3+0) only. FINDINGS.md says how each
// address was found and how sure that is.
//   HOLDERS      0x0802f0ac: 8 words, one per sound slot, each the slot's drum sample holder; holder+0 the object
//   object       +0 data, +0xd768 frames, +0xd76c words written by the last load, byte +0xd770 stereo, +0xd771 kind
//   CUR_SLOT     0x84a01528: the drum slot being edited
//   EDIT         0x11904754: the six-word parameter block the drum knob edit writes (+0 pitch, +4 start, +8 end,
//                +0xc ?, +0x10 mode bits, +0x14 volume/flags); logged as raw words
//   L1 WINDOW    0x11904700..0x11904800: the words around it (per-slot tables at 0x11904714 and 0x11904734)
//   DRUM VOICES  0x119062d8 + 0x38 i (i < 8): the first 14 words of each
// It never writes outside its own variables, runs only from the frame function every 16th frame, checks every
// address before reading, and logs a part only when it changed, at most once every 64 frames per part.

#include "build_m1_card.h"

typedef unsigned u32;
typedef unsigned char u8;

static const char LOG_FILE[] = "op1f/veffdrumprobe.cpp";

enum {
    SLOTS = 8, CHECK_EVERY = 16, LOG_GAP = 64,
    OFF_FRAMES = 0xd768, OFF_WORDS = 0xd76c, OFF_STEREO = 0xd770, OFF_KIND = 0xd771, OBJECT_BYTES = 0xd774,
    WIN_WORDS = 64, VOICES = 8, VOICE_WORDS = 14,
    PARTS = SLOTS + 1 + 1 + VOICES,                       // slots, edit block, window, voices
};
static const u32 HOLDERS = 0x0802f0acu, CUR_SLOT = 0x84a01528u;
static const u32 EDIT = 0x11904754u, WIN = 0x11904700u;
static const u32 VOICE_BASE = 0x119062d8u, VOICE_STRIDE = 0x38u;
static const u32 POOL = 0x8d518598u, POOL_STRIDE = 0x35d540u;
static const u32 DDR_LO = 0x80000000u, DDR_HI = 0x90000000u;
static const u32 SDRAM_LO = 0x08000000u, SDRAM_HI = 0x08100000u;
static const u32 L1_LO = 0x11900000u, L1_HI = 0x11910000u;

static inline u32 rd32(u32 a) { return *(const volatile u32*)a; }
static inline u32 rd8(u32 a) { return *(const volatile u8*)a; }
static inline int in(u32 a, u32 bytes, u32 lo, u32 hi) { return !(a & 3) && a >= lo && a < hi && bytes <= hi - a; }

static struct {
    u32 frames;
    u32 sig[PARTS];
    u32 said_at[PARTS];
    int said[PARTS];
} probe;

static u32 mix(u32 h, u32 v) { return (h ^ v) * 0x01000193u; }

// 1 when part p's signature is new and the gap since its last line has passed.
static int changed(u32 p, u32 sig) {
    if (probe.said[p] && probe.sig[p] == sig) return 0;
    if (probe.said[p] && probe.frames - probe.said_at[p] < (u32)LOG_GAP) return 0;
    probe.sig[p] = sig;
    probe.said[p] = 1;
    probe.said_at[p] = probe.frames;
    return 1;
}

static void check_slot(u32 slot, u32 ms) {
    u32 h = 0, o = 0, d = 0, fr = 0, wd = 0, st = 0, kd = 0, bad = 0;
    h = in(HOLDERS + slot * 4, 4, SDRAM_LO, SDRAM_HI) ? rd32(HOLDERS + slot * 4) : 0;
    if (!in(h, 4, DDR_LO, DDR_HI)) bad = 1;
    else {
        o = rd32(h);
        if (!in(o, OBJECT_BYTES, DDR_LO, DDR_HI)) bad = 2;
        else { d = rd32(o); fr = rd32(o + OFF_FRAMES); wd = rd32(o + OFF_WORDS); st = rd8(o + OFF_STEREO); kd = rd8(o + OFF_KIND); }
    }
    u32 sig = mix(mix(mix(mix(mix(mix(mix(mix(0x811c9dc5u, h), o), d), fr), wd), st), kd), bad);
    if (!changed(slot, sig)) return;
    if (bad) {
        op1f_log(2, LOG_FILE, 1, "drumprobe: slot %u check %u failed: holder %08x obj %08x; nothing behind it read", slot + 1, bad, h, o);
        return;
    }
    op1f_log(3, LOG_FILE, 2, "drumprobe: slot %u holder %08x obj %08x data %08x (%s) frames %u words %u stereo %u kind %u at %u ms",
             slot + 1, h, o, d, d == POOL + slot * POOL_STRIDE ? "pool" : "NOT pool", fr, wd, st, kd, ms);
}

static void check_edit(u32 ms) {
    u32 w[6], cur = in(CUR_SLOT, 4, DDR_LO, DDR_HI) ? rd32(CUR_SLOT) : 0xffffffffu, sig = mix(0x811c9dc5u, cur);
    for (u32 i = 0; i < 6; ++i) { w[i] = in(EDIT + i * 4, 4, L1_LO, L1_HI) ? rd32(EDIT + i * 4) : 0; sig = mix(sig, w[i]); }
    if (!changed(SLOTS, sig)) return;
    op1f_log(3, LOG_FILE, 3, "drumprobe: edit slot %u block %08x %08x %08x %08x %08x %08x (start %u end %u /2^31) at %u ms",
             cur + 1, w[0], w[1], w[2], w[3], w[4], w[5], w[1], w[2], ms);
}

// The L1 window, 64 words, in four lines of 16 when anything in it changed.
static void check_window(u32 ms) {
    u32 sig = 0x811c9dc5u;
    for (u32 i = 0; i < WIN_WORDS; ++i) sig = mix(sig, in(WIN + i * 4, 4, L1_LO, L1_HI) ? rd32(WIN + i * 4) : 0);
    if (!changed(SLOTS + 1, sig)) return;
    for (u32 q = 0; q < 4; ++q) {
        u32 a = WIN + q * 64, v[16];
        for (u32 i = 0; i < 16; ++i) v[i] = in(a + i * 4, 4, L1_LO, L1_HI) ? rd32(a + i * 4) : 0;
        op1f_log(3, LOG_FILE, 4, "drumprobe: w %08x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x at %u ms",
                 a, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], ms);
    }
}

static void check_voice(u32 i, u32 ms) {
    u32 a = VOICE_BASE + i * VOICE_STRIDE, v[VOICE_WORDS], sig = 0x811c9dc5u;
    if (!in(a, VOICE_WORDS * 4, L1_LO, L1_HI)) return;
    for (u32 k = 0; k < VOICE_WORDS; ++k) { v[k] = rd32(a + k * 4); if (k != 2) sig = mix(sig, v[k]); }   // +8 moves
    if (!changed(SLOTS + 2 + i, sig)) return;
    op1f_log(3, LOG_FILE, 5, "drumprobe: voice %u %x %x %x %x %x %x %x %x %x %x %x %x %x %x at %u ms",
             i, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], ms);
}

extern "C" void veffdrumprobe_frame(const Op1fFrame* now) OP1F_NAMED("veffdrumprobe_frame");
extern "C" void veffdrumprobe_frame(const Op1fFrame* now) {
    if (!now || now->bytes < (unsigned)sizeof(Op1fFrame)) return;
    ++probe.frames;
    if (probe.frames & (CHECK_EVERY - 1)) return;
    if (probe.frames == CHECK_EVERY)
        op1f_log(3, LOG_FILE, 6, "drumprobe: version 1, read-only; holders %08x edit %08x voices %08x", HOLDERS, EDIT, VOICE_BASE);
    for (u32 s = 0; s < SLOTS; ++s) check_slot(s, now->ms);
    check_edit(now->ms);
    check_window(now->ms);
    for (u32 i = 0; i < VOICES; ++i) check_voice(i, now->ms);
}
