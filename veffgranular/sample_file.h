// sample_file.h: the file source's reader. A WAV or AIFF/AIFC file read whole into a buffer is turned, in the same
// buffer, into 16-bit frames at its start (interleaved when stereo). No call of the unit's: the host test runs it on
// good, cut and random files. Display thread only (the frame function); the long-division below is a loop of shifts,
// since a pack links no division.
//
// Takes: PCM, 1 or 2 channels, 16, 24 or 32 bits, 8 to 192 kHz:
//   WAV  (RIFF/WAVE, format 1 or WAVE_FORMAT_EXTENSIBLE with PCM), root from a `smpl` chunk when there is one;
//   AIFF (big-endian) and AIFC with compression NONE (big-endian) or sowt (little-endian), root from `INST`.
// Anything else answers an error and leaves no frames.
#pragma once

namespace sfile {
typedef unsigned u32;
typedef unsigned char u8;

enum { OK = 0, E_SHORT = 1, E_FORM = 2, E_CODEC = 3, E_CHANNELS = 4, E_BITS = 5, E_RATE = 6, E_NODATA = 7, E_EMPTY = 8 };
static const char* const ERROR_TEXT[9] = { "ok", "too short", "not wav or aiff", "not pcm", "not 1 or 2 channels",
                                           "bits not 16/24/32", "rate not 8-192 khz", "no sound data", "empty" };

struct Info { u32 frames, stereo, rate, rate_q16, root; };

static inline u32 le16(const u8* p) { return (u32)p[0] | (u32)p[1] << 8; }
static inline u32 le32(const u8* p) { return le16(p) | le16(p + 2) << 16; }
static inline u32 be16(const u8* p) { return (u32)p[0] << 8 | (u32)p[1]; }
static inline u32 be32(const u8* p) { return be16(p) << 16 | be16(p + 2); }
static inline int tag(const u8* p, const char* t) { return p[0] == (u8)t[0] && p[1] == (u8)t[1] && p[2] == (u8)t[2] && p[3] == (u8)t[3]; }

// (a << 16) / b by shifts, for the rate's ratio (a < 2^16 * ... fits: a at most 192000)
static inline u32 div_q16(u32 a, u32 b) {
    u32 q = 0, r = 0;
    for (int bit = 47; bit >= 0; --bit) {
        r = (r << 1) | (bit >= 16 ? (a >> (bit - 16)) & 1u : 0u);
        if (r >= b) { r -= b; if (bit < 32) q |= 1u << bit; }
    }
    return q;
}

// the AIFF rate: an 80-bit extended number, read as a whole number of Hz
static inline u32 ext80(const u8* p) {
    const int e = (int)(((u32)(p[0] & 0x7f) << 8) | p[1]) - 16383;
    const u32 hi = be32(p + 2);
    if (p[0] & 0x80) return 0;
    if (e < 0 || e > 31) return 0;
    return hi >> (31 - e);
}

// The buffer `b` holds `n` bytes of a file. On OK, b starts with info.frames frames of 16-bit words.
static inline int parse(u8* b, u32 n, Info* info) {
    info->frames = info->stereo = info->rate = info->rate_q16 = 0;
    info->root = 60;
    if (n < 12u) return E_SHORT;
    const int wav = tag(b, "RIFF") && tag(b + 8, "WAVE");
    const int aif = tag(b, "FORM") && (tag(b + 8, "AIFF") || tag(b + 8, "AIFC"));
    if (!wav && !aif) return E_FORM;
    u32 channels = 0, bits = 0, rate = 0, data_at = 0, data_bytes = 0, little = wav, codec_ok = wav ? 0u : 1u;
    u32 at = 12;
    while (at + 8u <= n) {
        const u8* c = b + at;
        const u32 size = wav ? le32(c + 4) : be32(c + 4);
        const u32 body = at + 8u;
        const u32 room = n - body;
        const u32 have = size < room ? size : room;                       // a cut file: what is there
        if (wav && tag(c, "fmt ") && have >= 16u) {
            const u32 fmt = le16(c + 8);
            channels = le16(c + 10); rate = le32(c + 12); bits = le16(c + 22);
            codec_ok = fmt == 1u || (fmt == 0xfffeu && have >= 26u && le16(c + 32) == 1u);
        } else if (wav && tag(c, "data")) {
            data_at = body; data_bytes = have;
        } else if (wav && tag(c, "smpl") && have >= 36u) {
            const u32 note = le32(c + 8 + 12);
            if (note < 128u) info->root = note;
        } else if (aif && tag(c, "COMM") && have >= 18u) {
            channels = be16(c + 8); bits = be16(c + 14); rate = ext80(c + 16);
            if (tag(b + 8, "AIFC")) {
                if (have >= 22u && tag(c + 26, "NONE")) codec_ok = 1, little = 0;
                else if (have >= 22u && tag(c + 26, "sowt")) codec_ok = 1, little = 1;
                else codec_ok = 0;
            }
        } else if (aif && tag(c, "SSND") && have >= 8u) {
            data_at = body + 8u + be32(c + 8);                               // the offset before the frames
            data_bytes = data_at <= body + have ? body + have - data_at : 0u;
        } else if (aif && tag(c, "INST") && have >= 1u) {
            if (c[8] < 128u) info->root = c[8];
        }
        if (size > room) break;
        at = body + size + (size & 1u);                                      // chunks are padded to even
        if (at < body) break;                                                // wrapped: a broken size
    }
    if (!codec_ok) return E_CODEC;
    if (channels != 1u && channels != 2u) return E_CHANNELS;
    if (bits != 16u && bits != 24u && bits != 32u) return E_BITS;
    if (rate < 8000u || rate > 192000u) return E_RATE;
    if (!data_at) return E_NODATA;
    const u32 bytes = bits >> 3, frame_bytes = bytes * channels;
    u32 frames = 0;
    for (u32 left = data_bytes; left >= frame_bytes; left -= frame_bytes) ++frames;   // no division
    if (frames < 2u) return E_EMPTY;
    if (frames > (1u << 22)) frames = 1u << 22;
    // in place, front to back: each word written lies at or before the bytes it came from (2 <= bytes: 8-bit files are refused, they would grow)
    short* o = (short*)b;
    const u8* p = b + data_at;
    const u32 words = frames * channels;
    for (u32 i = 0; i < words; ++i, p += bytes) {
        u32 v;
        if (little) v = (u32)p[bytes - 2] | (u32)p[bytes - 1] << 8;      // the top two bytes
        else v = (u32)p[0] << 8 | (u32)p[1];
        o[i] = (short)(unsigned short)v;
    }
    info->frames = frames;
    info->stereo = channels == 2u;
    info->rate = rate;
    info->rate_q16 = div_q16(rate, 44100u);
    return OK;
}
}  // namespace sfile
