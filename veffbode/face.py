#!/usr/bin/env python3
"""bode's faceplate (veffbodepage.svg): the instructions bode.svg.make, and a preview.

  python3 -I veff/bode/face.py make                 writes veff/bode/bode.svg.make
  python3 -I veff/bode/face.py preview <tempo.svg> <out.png>   the whole picture, texts faked, through ImageMagick

The page: "veff bode shifter" with a wave logo, the side icon (two rings) top right; in the middle a sphere turning
(pink to violet, the Veff colours), a dotted circle and crosshair, the purple and cyan waves weaving through it; a
spectrum comb at -fs and at +fs, each with its marker (orange when that side sounds), moved apart as the shift grows;
below, four columns SHIFT / FINE / FEEDBACK / MIX: an arc of the value, the label and the value (op1f_text).

Only what the unit's own pictures hold: path commands M L C z, circles and lines, colours found in the unit's
own screens (content/display), stroke widths 1, 1.3 and 2. No arcs, no ids the stock has.
"""
import math, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPO_LEN, HEAD, TAIL = 43996, 371, 7

WHITE, GREY, DIM, DARK = '#CDD0C2', '#A2A4A6', '#5B6670', '#373D42'
PINKS = ['#5F4270', '#837CC7', '#AEB1DC', '#CC6699', '#EA7AC2', '#FFB7B3']   # back (violet) .. front (powder pink)
CYAN, PURPLE, ORANGE = '#35D2BA', '#837CC7', '#F15A29'

CX, CY, R = 240, 98, 36
WAVE_FRAMES, SPHERE_FRAMES, ARC_STEPS = 12, 8, 16


def f(v): return ('%.1f' % v).rstrip('0').rstrip('.')


def poly(pts):
    return 'M' + 'L'.join('%s,%s' % (f(x), f(y)) for x, y in pts)


def path(d, stroke, width='1.3', fill='none'):
    return '<path fill="%s" stroke="%s" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round" d="%s"/>' % (fill, stroke, width, d)


def line(x1, y1, x2, y2, stroke, width='0.7'):
    return '<line fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" x1="%s" y1="%s" x2="%s" y2="%s"/>' % (stroke, width, f(x1), f(y1), f(x2), f(y2))


def dot(x, y, r, colour):
    return '<circle fill="%s" cx="%s" cy="%s" r="%s"/>' % (colour, f(x), f(y), f(r))


def ring(x, y, r, colour, width='1.3'):
    return '<circle fill="none" stroke="%s" stroke-width="%s" cx="%s" cy="%s" r="%s"/>' % (colour, width, f(x), f(y), f(r))


def ellipse(cx, cy, rx, ry):   # four cubic quarters: no arcs
    k = 0.5523
    return ('M%s,%sC%s,%s %s,%s %s,%sC%s,%s %s,%s %s,%sC%s,%s %s,%s %s,%sC%s,%s %s,%s %s,%sz' % tuple(map(f, (
        cx + rx, cy, cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry,
        cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy,
        cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry,
        cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy))))


def holder(gid, x, y, colour):   # a text's place (op1f_text): its top-left and its colour
    return '<g id="%s"><path fill="none" stroke="%s" stroke-width="1.3" stroke-linecap="square" d="M%d,%dV%d"/></g>' % (gid, colour, x, y + 15, y)


def group(gid, parts):
    return '<g id="%s">%s</g>' % (gid, ''.join(parts))


def body():
    out = []
    # ---- head: logo, title, side icon
    logo = [(20 + i * 28 / 24, 22 - 6 * math.sin(2 * math.pi * i / 24) * math.cos(math.pi * i / 24)) for i in range(25)]
    out.append(path(poly(logo), PINKS[2]))
    out.append(path(poly([(x, 2 * 22 - y) for x, y in logo]), PINKS[4]))
    out.append(holder('bode_title', 58, 13, WHITE))
    for s, (a, b, ca, cb) in enumerate([(434, 446, DIM, PINKS[4]), (434, 446, PINKS[4], DIM),
                                        (434, 446, PINKS[4], PINKS[4])]):
        out.append(group('bsd%d' % s, [ring(a, 22, 9, ca), ring(b, 22, 9, cb)]))
    # ---- the middle: dotted circle, crosshair
    out += [dot(CX + 50 * math.cos(2 * math.pi * i / 48), CY + 50 * math.sin(2 * math.pi * i / 48), 0.8, GREY) for i in range(48)]
    out += [dot(x, CY, 0.6, DIM) for x in range(CX - 62, CX + 63, 4) if abs(x - CX) > 2]
    out += [dot(CX, y, 0.6, DIM) for y in range(CY - 52, CY + 53, 4) if abs(y - CY) > 2]
    # ---- the waves, a frame each (the pack shows one)
    for fr in range(WAVE_FRAMES):
        ph = 2 * math.pi * fr / WAVE_FRAMES
        xs = [146 + i * 4 for i in range(48)]
        a = [(x, CY - 20 * math.sin(2 * math.pi * (x - CX) / 104 + ph)) for x in xs]
        b = [(x, CY + 20 * math.sin(2 * math.pi * (x - CX) / 104 + ph)) for x in xs]
        out.append(group('bw_%d' % fr, [path(poly(a), PURPLE, '2'), path(poly(b), CYAN, '2')]))
    # ---- the sphere: latitudes still, meridians turning (front powder pink, back violet)
    out.append(ring(CX, CY, R, PINKS[1], '2'))
    for lat, col in ((-24, PINKS[1]), (-12, PINKS[2]), (0, PINKS[3]), (12, PINKS[4]), (24, PINKS[5])):
        rx = math.sqrt(R * R - lat * lat)
        out.append(path(ellipse(CX, CY + lat, rx, rx * 0.18), col, '0.7'))
    for fr in range(SPHERE_FRAMES):
        parts = []
        for k in range(6):
            th = math.radians(k * 30 + fr * 30 / SPHERE_FRAMES)
            rx = abs(R * math.cos(th))
            depth = math.sin(th)                                        # -1 back .. 1 front
            col = PINKS[min(5, int((depth + 1) / 2 * 6))]
            parts.append(path(ellipse(CX, CY, max(rx, 0.6), R), col, '1.3' if depth > 0 else '0.7'))
        out.append(group('bs_%d' % fr, parts))
    out.append(dot(CX, CY, 1.8, CYAN))
    arrow = 'M%d,%dC%d,%d %d,%d %d,%d' % (CX - 20, CY - 18, CX - 10, CY - 30, CX + 10, CY - 30, CX + 20, CY - 18)
    out.append(path(arrow, WHITE))
    out.append(path('M%d,%dL%d,%dL%d,%d' % (CX + 14, CY - 19, CX + 20, CY - 18, CX + 21, CY - 24), WHITE))
    # ---- the two combs (the pack moves them with op1f_move); a marker each, orange when that side sounds
    for side, cx in (('l', 92), ('r', 388)):
        parts = []
        for i in range(-9, 10):
            if i == 0: continue
            h = 38 * math.exp(-(i / 3.6) ** 2) * (1 if i % 2 else 0.8)
            parts.append(line(cx + 5 * i, 124, cx + 5 * i, 124 - h, WHITE) if h > 2 else dot(cx + 5 * i, 124, 0.7, GREY))
        parts.append(group('bm%s_on' % side, [line(cx, 124, cx, 78, ORANGE, '2')]))
        parts.append(group('bm%s_off' % side, [line(cx, 124, cx, 78, DIM, '2')]))
        out.append(group('bc%s' % side, parts))
    out.append(holder('bode_fl', 80, 130, GREY))
    out.append(holder('bode_fr', 376, 130, GREY))
    # ---- the four columns
    for c in range(4):
        x0 = c * 120
        if c: out.append(line(x0, 166, x0, 208, DIM))
        ax, ay, ar = x0 + 26, 190, 13
        def arc(frac):
            n = max(2, int(28 * frac) + 1)
            return [(ax + ar * math.cos(math.radians(135 + 270 * frac * i / (n - 1))),
                     ay + ar * math.sin(math.radians(135 + 270 * frac * i / (n - 1)))) for i in range(n)]
        out.append(path(poly(arc(1)), DARK, '2'))
        colour = ORANGE if c == 3 else WHITE
        for s in range(1, ARC_STEPS + 1):
            out.append(group('ba%d_%d' % (c, s), [path(poly(arc(s / ARC_STEPS)), colour, '2')]))
        out.append(holder('bode_l%d' % c, x0 + 48, 166, GREY))
        out.append(holder('bode_v%d' % c, x0 + 48, 186, WHITE))
    return '\n'.join(out)


def arc_pts(cx, cy, r, a0, a1, n):   # a polyline along a circle, degrees, screen orientation
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


# ---- a small stroke font of our own (the firmware's letters are too big for the columns): a 4 x 6 grid, y down,
# x-height from 2, descenders to 8. Each glyph is polylines.
FONT = {
    '0': [[(0,0),(4,0),(4,6),(0,6),(0,0)]], '1': [[(1,1),(2,0),(2,6)],[(1,6),(3,6)]],
    '2': [[(0,0),(4,0),(4,3),(0,3),(0,6),(4,6)]], '3': [[(0,0),(4,0),(4,6),(0,6)],[(1,3),(4,3)]],
    '4': [[(0,0),(0,3),(4,3)],[(4,0),(4,6)]], '5': [[(4,0),(0,0),(0,3),(4,3),(4,6),(0,6)]],
    '6': [[(4,0),(0,0),(0,6),(4,6),(4,3),(0,3)]], '7': [[(0,0),(4,0),(4,6)]],
    '8': [[(0,0),(4,0),(4,6),(0,6),(0,0)],[(0,3),(4,3)]], '9': [[(4,3),(0,3),(0,0),(4,0),(4,6),(0,6)]],
    '+': [[(0,3),(4,3)],[(2,1),(2,5)]], '-': [[(0.5,3),(3.5,3)]], '.': [[(1.6,6),(2.2,6)]],
    '%': [[(0,6),(4,0)],[(0,0),(1,0),(1,1),(0,1),(0,0)],[(3,5),(4,5),(4,6),(3,6),(3,5)]], '/': [[(0,6),(4,0)]],
    'a': [[(0,2),(4,2),(4,6),(0,6),(0,4),(4,4)]], 'b': [[(0,0),(0,6),(4,6),(4,2),(0,2)]],
    'c': [[(4,2),(0,2),(0,6),(4,6)]], 'd': [[(4,0),(4,6),(0,6),(0,2),(4,2)]],
    'e': [[(0,4),(4,4),(4,2),(0,2),(0,6),(4,6)]], 'f': [[(4,0),(2,0),(1,1),(1,6)],[(0,2),(3,2)]],
    'g': [[(4,6),(0,6),(0,2),(4,2),(4,8),(0,8)]], 'h': [[(0,0),(0,6)],[(0,2),(4,2),(4,6)]],
    'i': [[(2,2),(2,6)],[(2,0),(2,0.6)]], 'k': [[(0,0),(0,6)],[(4,2),(0,4.5),(4,6)]],
    'l': [[(1,0),(2,0),(2,6),(3,6)]], 'm': [[(0,6),(0,2),(4,2),(4,6)],[(2,2),(2,6)]],
    'n': [[(0,6),(0,2),(4,2),(4,6)]], 'o': [[(0,2),(4,2),(4,6),(0,6),(0,2)]],
    'p': [[(0,8),(0,2),(4,2),(4,6),(0,6)]], 'r': [[(0,6),(0,2),(4,2)]],
    's': [[(4,2),(0,2),(0,4),(4,4),(4,6),(0,6)]], 't': [[(1,0),(1,6),(4,6)],[(0,2),(3,2)]],
    'u': [[(0,2),(0,6),(4,6),(4,2)]], 'v': [[(0,2),(2,6),(4,2)]], 'w': [[(0,2),(0,6),(4,6),(4,2)],[(2,4),(2,6)]],
    'x': [[(0,2),(4,6)],[(4,2),(0,6)]], 'y': [[(0,2),(0,6),(4,6)],[(4,2),(4,8),(0,8)]], 'z': [[(0,2),(4,2),(0,6),(4,6)]],
}
GLYPHS = '0123456789+-.%/mshzupdownbtx'     # what a value can hold; bode.cpp keeps the same string
VALUE_CHARS, VALUE_SCALE, LABEL_SCALE = 8, 1.5, 1.1


def text_paths(t, x, y, scale, colour, width='1.3', gap=6):
    parts = []
    for i, ch in enumerate(t):
        for pl in FONT.get(ch, []):
            parts.append(path(poly([(x + (i * gap + px) * scale, y + py * scale) for px, py in pl]), colour, width))
    return parts


def body_future():
    """bode2: the reference's look. A glass orb of swirling rings (violet rim left, cyan rim right, powder pink core)
    turning frame by frame, thin glowing waves, a wide dotted halo, fine combs with a tall orange marker, open gauges."""
    out = []
    CXF, CYF, RF = 240, 96, 42
    VIOLET_DEEP, GLOW_CYAN = '#5F4270', '#0F494C'
    # ---- head: the logo (two crossing loops), the title spaced out, two rings for the side
    lo = [(22 + i * 26 / 32, 22 + 6 * math.sin(2 * math.pi * i / 32)) for i in range(33)]
    out.append(path(poly(lo), PINKS[2]))
    out.append(path(poly([(x, 44 - y) for x, y in lo]), WHITE))
    out += text_paths('veffbode', 60, 15, 1.7, WHITE, '1.3', 7)
    for s, (ca, cb) in enumerate([(DIM, WHITE), (WHITE, DIM), (WHITE, WHITE)]):
        out.append(group('bsd%d' % s, [ring(432, 22, 9, ca), ring(446, 22, 9, cb)]))
    # ---- the halo: a dotted circle and a dotted crosshair
    out += [dot(CXF + 62 * math.cos(2 * math.pi * i / 64), CYF + 62 * math.sin(2 * math.pi * i / 64), 0.8, WHITE) for i in range(64)]
    out += [dot(x, CYF, 0.6, GREY) for x in range(CXF - 76, CXF + 77, 4) if abs(x - CXF) > 3]
    out += [dot(CXF, y, 0.6, GREY) for y in range(CYF - 70, CYF + 71, 4) if abs(y - CYF) > 3]
    # ---- the waves: a dark wide stroke under a thin bright one (a glow), crossing at the orb
    for fr in range(WAVE_FRAMES):
        ph = 2 * math.pi * fr / WAVE_FRAMES
        xs = [128 + i * 4 for i in range(57)]
        env = lambda x: 1 - 0.25 * math.exp(-((x - CXF) / 40.0) ** 2)
        a = [(x, CYF + 24 * env(x) * math.sin(2 * math.pi * (x - CXF) / 116 + ph)) for x in xs]
        b = [(x, CYF - 24 * env(x) * math.sin(2 * math.pi * (x - CXF) / 116 + ph)) for x in xs]
        out.append(group('bw_%d' % fr, [path(poly(a), VIOLET_DEEP, '3'), path(poly(b), GLOW_CYAN, '3'),
                                         path(poly(a), PINKS[2], '1.3'), path(poly(b), CYAN, '1.3')]))
    # ---- the orb: rims still, the swirl of rings turning
    out.append(path(poly(arc_pts(CXF, CYF, RF, 95, 265, 40)), PINKS[1], '2'))      # violet rim, left
    out.append(path(poly(arc_pts(CXF, CYF, RF, -85, 85, 40)), CYAN, '2'))          # cyan rim, right
    out.append(path(poly(arc_pts(CXF, CYF, RF - 2, 100, 260, 40)), VIOLET_DEEP, '0.7'))
    rings = [(0.92, PINKS[1]), (0.82, PINKS[2]), (0.72, PINKS[3]), (0.62, PINKS[4]), (0.52, PINKS[5]), (0.42, PINKS[4]), (0.32, PINKS[2])]
    for fr in range(SPHERE_FRAMES):
        parts = []
        for k, (scale, col) in enumerate(rings):
            r = RF * scale
            off = (RF - r) * 0.7                                       # each ring leans toward a side that turns
            th = 2 * math.pi * fr / SPHERE_FRAMES + k * 0.16
            cx, cy = CXF + off * math.cos(th), CYF + off * 0.6 * math.sin(th)
            parts.append(path(ellipse(cx, cy, r, r * 0.96), col, '0.7' if k % 2 else '1.3'))
        out.append(group('bs_%d' % fr, parts))
    out.append(dot(CXF, CYF, 2, CYAN))
    out.append(path('M%d,%dC%d,%d %d,%d %d,%d' % (CXF - 22, CYF - 22, CXF - 12, CYF - 34, CXF + 12, CYF - 34, CXF + 22, CYF - 22), WHITE))
    out.append(path('M%d,%dL%d,%dL%d,%d' % (CXF + 15, CYF - 22, CXF + 22, CYF - 22, CXF + 23, CYF - 29), WHITE))
    # ---- the combs: fine bars on a dotted floor, a tall orange marker
    for side, cx in (('l', 84), ('r', 396)):
        parts = [dot(cx + 3 * i, 122, 0.6, GREY) for i in range(-16, 17)] + text_paths('-fs' if side == 'l' else '+fs', cx - 10, 128, 1.2, WHITE)
        for i in range(-8, 9):
            if i == 0: continue
            h = 34 * math.exp(-(i / 3.2) ** 2)
            if h > 3: parts.append(line(cx + 6 * i, 122, cx + 6 * i, 122 - h, WHITE, '1.3'))
        parts.append(group('bm%s_on' % side, [line(cx, 122, cx, 66, ORANGE, '1.3')]))
        parts.append(group('bm%s_off' % side, [line(cx, 122, cx, 66, DIM, '1.3')]))
        out.append(group('bc%s' % side, parts))
    # ---- the gauges: open at the bottom, a dark track, the value in white (mix in orange)
    for c in range(4):
        x0 = c * 120
        if c: out.append(line(x0, 170, x0, 210, DIM, '0.7'))
        ax, ay, ar = x0 + 24, 192, 14
        out.append(path(poly(arc_pts(ax, ay, ar, 150, 390, 30)), DARK, '2'))
        colour = ORANGE if c == 3 else WHITE
        for s in range(1, ARC_STEPS + 1):
            out.append(group('ba%d_%d' % (c, s), [path(poly(arc_pts(ax, ay, ar, 150, 150 + 240 * s / ARC_STEPS, max(2, 2 * s))), colour, '2')]))
        for pg, labs in enumerate((('shift', 'phase', 'feedback', 'mix'), ('delay', 'sideband', 'tone', 'smear'))):
            out.append(group('bl%d_%d' % (c, pg), text_paths(labs[c], x0 + 44, 172, LABEL_SCALE, GREY, '1.3', 6)))
        for i in range(VALUE_CHARS):
            for g, ch in enumerate(GLYPHS):
                out.append(group('t%d%d_%d' % (c, i, g), text_paths(ch, x0 + 44 + i * 6 * VALUE_SCALE, 188, VALUE_SCALE, WHITE, '1.3')))
    return '\n'.join(out)


STYLES = {'veffbode': (body_future, 'veffbode.svg.make')}


def make(style='veffbode'):
    fn, name = STYLES[style]
    b = fn().encode()
    text = ('op1f picture 2\nsize %d\nfrom tempo.svg %d eb050950\nc 0 0 %d\na %d\n' % (HEAD + len(b) + TAIL, TEMPO_LEN, HEAD, len(b))).encode() \
        + b + ('\nc 0 %d %d\n' % (TEMPO_LEN - TAIL, TAIL)).encode()
    (HERE / name).parent.mkdir(exist_ok=True)
    (HERE / name).write_bytes(text)
    print('wrote %s: picture of %d bytes' % (name, HEAD + len(b) + TAIL))


def preview(tempo, png, style='veffbode'):
    t = Path(tempo).read_bytes()
    b = STYLES[style][0]()
    import re
    vals = sys.argv[5:9] if len(sys.argv) > 8 else ['+120hz', '0', '67%', '35%']
    show = {'bsd0', 'bw_3', 'bs_2', 'bml_off', 'bmr_on', 'ba0_9', 'ba1_8', 'ba2_12', 'ba3_6'} | {'bl%d_0' % c for c in range(4)}
    for c, v in enumerate(vals):
        for k, ch in enumerate(v): show.add('t%d%d_%d' % (c, k, GLYPHS.index(ch)))
    def keep(m):
        gid = m.group(1)
        if re.match(r'(bsd\d|bw_\d+|bs_\d|bm[lr]_o\w+|ba\d_\d+|bl\d_\d|t\d\d_\d+)$', gid) and gid not in show: return ''
        return m.group(0)
    b = re.sub(r'<g id="([^"]+)">(?:(?!<g id=).)*?</g>', keep, b)
    svg = t[:HEAD] + b"<rect x='0' y='0' width='480' height='220' fill='#000005'/>" + b.encode() + t[-TAIL:]
    svg_path = Path(png).with_suffix('.svg')
    svg_path.write_bytes(svg)
    html = Path(png).with_suffix('.html')
    html.write_text('<html><body style="margin:0;background:#000"><img src="%s" style="width:960px;height:440px"></body></html>' % svg_path.name)
    chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
    subprocess.run([chrome, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--window-size=960,440',
                    '--screenshot=%s' % png, 'file://%s' % html], check=True, capture_output=True)
    print('wrote', png)


if __name__ == '__main__':
    if sys.argv[1] == 'make': make(*sys.argv[2:3])
    else: preview(*sys.argv[2:5])
