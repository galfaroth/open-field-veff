"""Writes veffurator_tables.h: run `python3.12 -I veff/veffurator/make_tables.py` from the checkout.

The six shaper curves (1025 points each over |u| = 0 .. 8, odd, Q31, slope 1 at 0), the drive and output gains,
the colour filter's coefficients (a TPT state-variable band-pass, k = 1, 100 Hz .. 10 kHz in 61 steps) and the
2x oversampler's half-band FIR (19 taps, Kaiser window).
"""
import math, os

POINTS, DOMAIN = 1025, 8.0
FULL = 2**31 - 1
SR = 44100.0


def q31(v):
    return max(-FULL - 1, min(FULL, int(round(v * 2**31))))


def curve(kind, u):
    if kind == "analog": return math.tanh(u)
    if kind == "sine": return math.sin(u) if u < math.pi / 2 else 1.0
    if kind == "medium": return u / (1 + u**4) ** 0.25
    if kind == "hard": return u / (1 + u**10) ** 0.1
    if kind == "fold": return math.sin(u)
    if kind == "digital": return min(u, 1.0)
    raise ValueError(kind)


CURVES = ["analog", "sine", "medium", "hard", "fold", "digital"]


def kaiser(n, N, beta):
    def i0(x):
        s, t, k = 1.0, 1.0, 1
        while t > 1e-12:
            t *= (x / 2 / k) ** 2; s += t; k += 1
        return s
    r = 2 * n / (N - 1) - 1
    return i0(beta * math.sqrt(max(0.0, 1 - r * r))) / i0(beta)


def halfband(P=5, beta=6.0):
    N = 4 * P - 1; C = N // 2
    h = []
    for j in range(N):
        m = j - C
        s = 0.5 if m == 0 else math.sin(math.pi * m / 2) / (math.pi * m)
        h.append(s * kaiser(j, N, beta))
    side = sum(h) - h[C]
    k = 0.5 / side                       # the side taps sum to 0.5: DC passes at exactly 1
    return [h[2 * i] * k for i in range(P)]


def main():
    out = []
    w = out.append
    w("// Made by veff/veffurator/make_tables.py: do not edit by hand.")
    w("enum { CURVES = %d, CURVE_POINTS = %d, DRIVE_STEPS = 73, OUT_STEPS = 37, FREQ_STEPS = 61, HB_TAPS = 5 };" % (len(CURVES), POINTS))
    w("static const int CURVE[%d][%d] = {" % (len(CURVES), POINTS))
    for kind in CURVES:
        vals = [q31(curve(kind, DOMAIN * i / (POINTS - 1))) for i in range(POINTS)]
        w("  { // " + kind)
        for i in range(0, POINTS, 8):
            w("    " + ", ".join(str(v) for v in vals[i:i + 8]) + ",")
        w("  },")
    w("};")
    w("// drive -36 .. +36 dB: the gain / 64 in Q31")
    w("static const int DRIVE_GAIN[73] = { " + ", ".join(str(q31(10 ** ((d - 36) / 20) / 64)) for d in range(73)) + " };")
    w("// output -36 .. 0 dB in Q31")
    w("static const int OUT_GAIN[37] = { " + ", ".join(str(q31(10 ** ((d - 36) / 20))) for d in range(37)) + " };")
    w("// colour: TPT SVF a1, a2, a3 (k = 1) for 100 Hz * 100^(s/60), and the frequency in Hz for the page")
    rows, hz = [], []
    for s in range(61):
        f = 100 * 100 ** (s / 60); hz.append(int(round(f)))
        g = math.tan(math.pi * f / SR)
        a1 = 1 / (1 + g * (g + 1)); a2 = g * a1; a3 = g * a2
        rows.append("{ %d, %d, %d }" % (q31(a1), q31(a2), q31(a3)))
    w("static const int COLOR_SVF[61][3] = { " + ", ".join(rows) + " };")
    w("static const unsigned short COLOR_HZ[61] = { " + ", ".join(map(str, hz)) + " };")
    hb = halfband()
    w("// the half-band's side taps h[0], h[2], .. h[8] (Q31); the centre is 0.5")
    w("static const int HB[5] = { " + ", ".join(str(q31(v)) for v in hb) + " };")
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "veffurator_tables.h"), "w") as fh:
        fh.write("\n".join(out) + "\n")


main()
