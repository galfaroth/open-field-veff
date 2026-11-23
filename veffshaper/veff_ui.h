// veff_ui.h: the common Veff page (after the Veff reference sheet), painted every frame on the base's blank page.
//
//   │ VEFF NAME                              icon │   title: "VEFF" pink, the name white, wide letter spacing
//   │        ✦      the effect's own picture   ✦  │   band VEFF_ART_TOP..VEFF_ART_BOTTOM, axis at VEFF_AXIS_Y
//   │ ● LABEL    │ ● LABEL    │ ● LABEL  │ ● LABEL │   four columns: pink dot, label, value
//   │   value    │   value    │   value  │   value │
//   │ ▬▬▬▬|────  │ ▬▬▬|─────  │ ...      │ ...     │   slider: pink to the value, white after, white tick
//
// Copied unchanged into every Veff pack's folder (from veff/common/), so each pack builds on its own.
// Main thread only (frame functions). Integer maths, no division, no float. Letters: Veff's stroke font, after
// veffbode's face.py, drawn with op1f_paint_line.
#pragma once

namespace veffui {

static inline unsigned rgb(int r, int g, int b) { return OP1F_COLOUR(r, g, b); }
// a colour at a = 0..256 of its strength over black (the page is black, so this is how "transparent" looks)
static inline unsigned dim(int r, int g, int b, int a) { return OP1F_COLOUR((r * a) >> 8, (g * a) >> 8, (b * a) >> 8); }

#define VEFF_PINK   OP1F_COLOUR(0xf4, 0x67, 0xb3)
#define VEFF_WHITE  OP1F_COLOUR(0xec, 0xeb, 0xe5)
#define VEFF_GREY   OP1F_COLOUR(0x85, 0x83, 0x8b)
#define VEFF_DARK   OP1F_COLOUR(0x3a, 0x39, 0x3e)
static inline unsigned pink(int a) { return dim(0xf4, 0x67, 0xb3, a); }
static inline unsigned white(int a) { return dim(0xec, 0xeb, 0xe5, a); }

enum {
    VEFF_W = 480, VEFF_H = 224,
    VEFF_FRAME_X = 4, VEFF_FRAME_Y = 4, VEFF_FRAME_W = 472, VEFF_FRAME_H = 216, VEFF_FRAME_R = 12,
    VEFF_TITLE_X = 12, VEFF_TITLE_Y = 12,
    VEFF_ART_LEFT = 8, VEFF_ART_RIGHT = 472, VEFF_ART_TOP = 38, VEFF_ART_BOTTOM = 146, VEFF_AXIS_Y = 92, VEFF_MID_X = 240,
    VEFF_COL_X = 12, VEFF_COL_W = 117,      // columns at 12, 129, 246, 363
    // every picture follows the same frame: the axis from VEFF_AXIS_L to VEFF_AXIS_R, the drawing within the same x,
    // its peaks at most VEFF_ART_HALF above or below the axis (typical settings reach ~0.75 of it)
    VEFF_AXIS_L = 24, VEFF_AXIS_R = 456, VEFF_ART_HALF = 50,
    VEFF_LABEL_Y = 156, VEFF_VALUE_Y = 170, VEFF_SLIDER_Y = 200, VEFF_SLIDER_W = 94,
};

// ---- the stroke font. A glyph is pairs of letters 'a'+x, 'a'+y on a grid 8 wide and 12 tall (caps and digits
// from y 0, small letters from y 4, descenders to y 16); a space lifts the pen.
static const char* const FONT_UPPER[26] = {
    "ameaim cggg", "aagaicieggag aaamgmikiigg", "icgacaacakcmgmik", "aafaidijfmamaa", "iaaaamim aggg",
    "iaaaam aggg", "icgacaacakcmgmikigeg", "aaam iaim agig", "eaem caga cmgm", "iaikgmcmak",
    "aaam iaai dfim", "aaamim", "amaaehiaim", "amaaimia", "cagaicikgmcmakacca",
    "amaagaicieggag", "cagaicikgmcmakacca fjim", "amaagaicieggag egim", "icgacaacaecgggiiikgmcmak", "aaia eaem",
    "aaakcmgmikia", "aaemia", "aacmefgmia", "aaim iaam", "aaegia egem", "aaiaamim",
};
static const char* const FONT_LOWER[26] = {
    "aeieimamaiii", "aaamimieae", "ieaeamim", "iaimamaeie", "aiiiieaeamim", "iaeacccm aege", "imamaeieiqaq",
    "aaam aeieim", "eeem eaeb", "gegqcq gagb", "aaam ieajim", "caeaemgm", "amaeieim eeem", "amaeieim",
    "aeieimamae", "aqaeieimam", "iqieaeamim", "amaeie", "ieaeaiiiimam", "cacmim aege", "aeamimie", "aeemie",
    "aeamimie eiem", "aeim ieam", "aeamim ieiqaq", "aeieamim",
};
static const char* const FONT_DIGIT[10] = {
    "cagaicikgmcmakacca", "cceaem cmgm", "accagaicieamim", "accagaicieggdg ggiiikgmcmak", "gmgaaiii",
    "iaaaafgfihikgmcmak", "hadaaeakcmgmikiiggcgai", "aaiadm", "cgaeaccagaicieggcgaiakcmgmikiigg",
    "ieggcgaeaccagaiciifmbm",
};

static inline const char* glyph(char c) {
    if (c >= 'A' && c <= 'Z') return FONT_UPPER[c - 'A'];
    if (c >= 'a' && c <= 'z') return FONT_LOWER[c - 'a'];
    if (c >= '0' && c <= '9') return FONT_DIGIT[c - '0'];
    switch (c) {
        case '+': return "agig ecek";
        case '-': return "bghg";
        case '.': return "dmem";
        case ':': return "edee ekel";
        case '/': return "amia";
        case '%': return "amia aacaccacaa gkikimgmgk";
        case '#': return "cacm gagm aeie aiii";
        case '\'': return "eaec";
        default: return "";
    }
}
static inline int narrow(char c) { return c == '.' || c == ':' || c == '\'' || c == ' '; }

// s: quarter pixels per grid unit (caps are 12 units: s 2 = 6 px, s 3 = 9 px, s 4 = 12 px); track: extra px between
// letters. Answers the width. colour 0 measures only.
static inline int text(void* canvas, int x, int y, const char* t, unsigned colour, int s, int track, int pen = 1) {
    int at = x;
    for (; *t; ++t) {
        const char* g = glyph(*t);
        int px = 0, py = 0, have = 0;
        if (canvas && colour) {
            for (const char* p = g; *p; ) {
                if (*p == ' ') { have = 0; ++p; continue; }
                const int gx = p[0] - 'a', gy = p[1] - 'a';
                const int nx = at + ((gx * s + 2) >> 2), ny = y + ((gy * s + 2) >> 2);
                if (have) op1f_paint_line(canvas, px, py, nx, ny, pen, colour);
                px = nx; py = ny; have = 1; p += 2;
            }
        }
        at += (((narrow(*t) ? 4 : 8) * s + 2) >> 2) + track + ((3 * s) >> 2);
    }
    return at - x - track - ((3 * s) >> 2);
}
static inline int text_width(const char* t, int s, int track) { return text(0, 0, 0, t, 0, s, track); }

// ---- shapes
static const short SIN64[65] = {       // sin, 0..90 degrees in 64 steps, Q14
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897, 6270, 6639, 7005,
    7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297, 11585, 11866, 12140, 12406,
    12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557,
    15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384 };
// sine of a turn in 256 steps, Q14
static inline int sin256(int a) {
    a &= 255;
    const int q = a >> 6, r = a & 63;
    const int v = (q & 1) ? SIN64[64 - r] : SIN64[r];
    return q >= 2 ? -v : v;
}
static inline int cos256(int a) { return sin256(a + 64); }

// a circle (or ellipse) as 1 << lg segments
static inline void ring(void* c, int cx, int cy, int rx, int ry, unsigned colour, int pen = 1, int lg = 5) {
    const int n = 1 << lg;
    int px = cx + rx, py = cy;
    for (int i = 1; i <= n; ++i) {
        const int a = (i << 8) >> lg;
        const int x = cx + ((rx * cos256(a)) >> 14), y = cy + ((ry * sin256(a)) >> 14);
        op1f_paint_line(c, px, py, x, y, pen, colour);
        px = x; py = y;
    }
}
static inline int isqrt(int v) { int r = 0; for (int b = 1 << 14; b; b >>= 1) if ((r + b) * (r + b) <= v) r += b; return r; }
// a filled disc, row by row
static inline void disc(void* c, int cx, int cy, int r, unsigned colour) {
    for (int dy = -r; dy <= r; ++dy) { const int w = isqrt(r * r - dy * dy); op1f_paint_box(c, cx - w, cy + dy, 2 * w + 1, 1, colour); }
}
static inline void dashed_v(void* c, int x, int y0, int y1, unsigned colour, int on = 4, int off = 4) {
    for (int y = y0; y < y1; y += on + off) op1f_paint_line(c, x, y, x, y + on - 1 < y1 ? y + on - 1 : y1, 1, colour);
}
// the four-point sparkle: size 3..8 px. It twinkles by itself with the page's frame tick (each at its own phase, from
// its place); tw 0..256 scales it further (256 = full).
static unsigned tick;                  // counted up by page() once a frame
static inline void sparkle(void* c, int x, int y, int size, unsigned colour, int tw = 256) {
    const int own = 128 + (sin256((int)(tick * 5u) + x * 7 + y * 13) >> 7);          // 0..256
    const int s = 1 + (((size - 1) * ((own * tw) >> 8)) >> 8);
    op1f_paint_line(c, x - s, y, x + s, y, 1, colour);
    op1f_paint_line(c, x, y - s, x, y + s, 1, colour);
    if (s > 2) op1f_paint_box(c, x - 1, y - 1, 3, 3, colour);
    else op1f_paint_box(c, x, y, 1, 1, colour);
}

// the page's sparkles: the same three places on every Veff page (two white, one pink), each twinkling on its own
static inline void sparkles(void* c) {
    sparkle(c, 112, VEFF_ART_TOP + 8, 6, VEFF_PINK);
    sparkle(c, 404, VEFF_ART_TOP + 2, 6, VEFF_WHITE);
    sparkle(c, 64, VEFF_ART_BOTTOM - 10, 5, VEFF_WHITE);
}
// the page's axis: thin white, VEFF_AXIS_L .. VEFF_AXIS_R at VEFF_AXIS_Y
static inline void axis(void* c, unsigned colour = VEFF_WHITE) {
    op1f_paint_line(c, VEFF_AXIS_L, VEFF_AXIS_Y, VEFF_AXIS_R, VEFF_AXIS_Y, 1, colour);
}

// ---- the frame: a rounded box
static inline void frame(void* c, unsigned colour) {
    const int x0 = VEFF_FRAME_X, y0 = VEFF_FRAME_Y, x1 = x0 + VEFF_FRAME_W - 1, y1 = y0 + VEFF_FRAME_H - 1, r = VEFF_FRAME_R;
    op1f_paint_line(c, x0 + r, y0, x1 - r, y0, 1, colour);
    op1f_paint_line(c, x0 + r, y1, x1 - r, y1, 1, colour);
    op1f_paint_line(c, x0, y0 + r, x0, y1 - r, 1, colour);
    op1f_paint_line(c, x1, y0 + r, x1, y1 - r, 1, colour);
    const int cxs[4] = { x1 - r, x0 + r, x0 + r, x1 - r }, cys[4] = { y1 - r, y1 - r, y0 + r, y0 + r };
    for (int q = 0; q < 4; ++q) {
        int px = cxs[q] + ((r * cos256(q * 64)) >> 14), py = cys[q] + ((r * sin256(q * 64)) >> 14);
        for (int i = 1; i <= 6; ++i) {
            const int a = q * 64 + ((i * 683) >> 6);
            const int x = cxs[q] + ((r * cos256(a)) >> 14), y = cys[q] + ((r * sin256(a)) >> 14);
            op1f_paint_line(c, px, py, x, y, 1, colour);
            px = x; py = y;
        }
    }
}

// ---- the icons, top right (box 40 x 24 at x 420, y 12)
enum { ICON_BODE, ICON_SHAPER, ICON_RRRR, ICON_URATOR, ICON_ROSION };
static inline void icon(void* c, int which, unsigned colour) {
    const int x = 432, y = 10;
    switch (which) {
        case ICON_BODE: ring(c, x + 13, y + 11, 11, 11, colour, 1, 5); ring(c, x + 25, y + 11, 11, 11, colour, 1, 5); break;
        case ICON_SHAPER: {
            int px = x, py = y + 22;
            for (int i = 1; i <= 16; ++i) {             // a smooth step up, a flat top, and down
                const int t = i * 16;                    // 0..256
                const int nx = x + ((i * 36) >> 4);
                const int ny = t < 96 ? y + 22 - ((22 * (16384 - cos256((t * 171) >> 7))) >> 15)
                             : t < 176 ? y : y + ((22 * (16384 - cos256(((t - 176) * 205) >> 7))) >> 15);
                op1f_paint_line(c, px, py, nx, ny, 1, colour);
                px = nx; py = ny;
            }
            break;
        }
        case ICON_RRRR: for (int i = 0; i < 5; ++i) op1f_paint_line(c, x + 4 + i * 7, y + 2, x + 4 + i * 7, y + 22, 1, colour); break;
        case ICON_URATOR:
            for (int k = 0; k < 2; ++k) {
                const int yy = y + 4 + k * 8;
                op1f_paint_line(c, x + 4, yy + 6, x + 20, yy, 1, colour); op1f_paint_line(c, x + 20, yy, x + 36, yy + 6, 1, colour);
                op1f_paint_line(c, x + 36, yy + 6, x + 20, yy + 12, 1, colour); op1f_paint_line(c, x + 20, yy + 12, x + 4, yy + 6, 1, colour);
            }
            break;
        case ICON_ROSION: {
            const int cx[3] = { x + 20, x + 13, x + 27 }, cy[3] = { y + 5, y + 17, y + 17 };
            static const signed char HX[7][2] = { {0,-7}, {6,-4}, {6,4}, {0,7}, {-6,4}, {-6,-4}, {0,-7} };
            for (int k = 0; k < 3; ++k) for (int i = 0; i < 6; ++i) op1f_paint_line(c, cx[k] + HX[i][0], cy[k] + HX[i][1], cx[k] + HX[i+1][0], cy[k] + HX[i+1][1], 1, colour);
            break;
        }
    }
}

// ---- the page: frame, title, icon, columns. value_frac: 0..1024 of each slider. tag: a short pink text after the
// title (the shift page or a sub-page), or 0.
struct Column { const char* label; const char* value; int frac; };
static inline void page(void* c, const char* name, int which_icon, const Column col[4], const char* tag, int on) {
    ++tick;
    int x = VEFF_TITLE_X;
    x += text(c, x, VEFF_TITLE_Y, "VEFF", VEFF_PINK, 3, 6) + 12;
    x += text(c, x, VEFF_TITLE_Y, name, VEFF_WHITE, 3, 6);
    if (tag) text(c, x + 14, VEFF_TITLE_Y + 3, tag, VEFF_PINK, 2, 3);
    if (!on) text(c, x + 14, VEFF_TITLE_Y + 3, "OFF", VEFF_GREY, 2, 3);
    icon(c, which_icon, VEFF_WHITE);
    for (int i = 1; i < 4; ++i) {
        const int dx = VEFF_COL_X + i * VEFF_COL_W - 7;
        op1f_paint_line(c, dx, VEFF_LABEL_Y - 4, dx, VEFF_SLIDER_Y - 4, 1, VEFF_WHITE);
    }
    for (int i = 0; i < 4; ++i) {
        const int x0 = VEFF_COL_X + i * VEFF_COL_W;
        if (!col[i].label || !*col[i].label) continue;
        disc(c, x0 + 11, VEFF_LABEL_Y + 3, 3, on ? VEFF_PINK : VEFF_GREY);
        text(c, x0 + 24, VEFF_LABEL_Y, col[i].label, VEFF_WHITE, 2, 2);
        const int room = VEFF_COL_W - 42;   // a value too wide for its column steps down a size
        const int vs = text_width(col[i].value, 4, 1) <= room ? 4 : 3;
        text(c, x0 + 24, VEFF_VALUE_Y + (vs == 4 ? 0 : 2), col[i].value, VEFF_WHITE, vs, 1);
        int f = col[i].frac; f = f < 0 ? 0 : f > 1024 ? 1024 : f;
        const int w = (VEFF_SLIDER_W * f) >> 10;
        if (w > 0) op1f_paint_box(c, x0, VEFF_SLIDER_Y - 1, w, 3, on ? VEFF_PINK : VEFF_GREY);
        if (w < VEFF_SLIDER_W) op1f_paint_box(c, x0 + w, VEFF_SLIDER_Y, VEFF_SLIDER_W - w, 2, VEFF_WHITE);
        op1f_paint_box(c, x0 + w - 1, VEFF_SLIDER_Y - 5, 2, 11, VEFF_WHITE);
    }
}

}  // namespace veffui
