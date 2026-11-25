# Writes veffgranular_tables.h: python3.12 -I veff/veffgranular/gen_tables.py > veff/veffgranular/veffgranular_tables.h
# Every number the render needs that would otherwise take a division, a power or a float.
import math

OUT_RATE = 88200.0                      # the engine's output rate (level 9)
SIZES = 101                             # the size value 0..100
DENS = 101                              # the density value 0..100


def arr(name, vals, per=8, typ="unsigned"):
    s = f"static const {typ} {name}[{len(vals)}] = {{\n"
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"


out = ["// Made by gen_tables.py in this folder: do not edit by hand.\n#pragma once\n\n"]

# ---- windows: 1025 points (index = phase >> 22, the last point for interpolation-free safety), Q15.
WINDOW_POINTS = 1025


def hann(x): return math.sin(math.pi * x) ** 2


def tukey(x, r=0.5):                    # flat top, cosine edges each r/2 of the grain
    e = r / 2
    if x < e: return 0.5 * (1 - math.cos(math.pi * x / e))
    if x > 1 - e: return 0.5 * (1 - math.cos(math.pi * (1 - x) / e))
    return 1.0


def perc(x):                            # a fast rise (5%) and an exponential fall to 0 at the end
    if x < 0.05: return math.sin(0.5 * math.pi * x / 0.05) ** 2
    y = (x - 0.05) / 0.95
    return math.exp(-4.0 * y) * (1 - y) ** 0.5


def tri(x): return 1 - abs(2 * x - 1)


def q15(v): return max(0, min(32767, int(round(v * 32767))))


shapes = [("soft", hann), ("flat", tukey), ("pluck", perc), ("tri", tri)]
win = []
for _, f in shapes:
    for i in range(WINDOW_POINTS):
        win.append(q15(f(min(1.0, i / (WINDOW_POINTS - 1)))))
out.append("// the window shapes, in the order the page names them: " + ", ".join(n for n, _ in shapes) + "\n")
out.append(f"enum {{ WINDOW_SHAPES = {len(shapes)}, WINDOW_POINTS = {WINDOW_POINTS} }};\n")
out.append(arr("WINDOW", win, 16, "unsigned short"))

# ---- grain size: 0..100 to 5 ms .. 1000 ms, exponential, in output frames; and the window's step, 2^32 / frames,
# rounded down so frames * step stays below 2^32 (the phase wraps only after the last frame).
size_ms = [5.0 * (200.0 ** (i / 100.0)) for i in range(SIZES)]
size_frames = [max(16, int(round(ms * OUT_RATE / 1000.0))) for ms in size_ms]
out.append("// grain size: output frames (88,200 a second) for the size value 0..100: 5 ms .. 1 s\n")
out.append(arr("SIZE_FRAMES", size_frames))
out.append("// 2^32 / SIZE_FRAMES, the window phase's step a frame\n")
out.append(arr("SIZE_WSTEP", [(2 ** 32 - 1) // f for f in size_frames]))
out.append("// size_ms * 10, for the page\n")
out.append(arr("SIZE_MS10", [int(round(ms * 10)) for ms in size_ms]))

# ---- density: 0..100 to an overlap of 0.5 .. 5 grains (grains sounding at once on average), exponential.
overlap = [0.5 * (10.0 ** (i / 100.0)) for i in range(DENS)]
out.append("// density: 65536 / overlap (overlap 0.5 .. 5): the spacing of grain starts is size * this >> 16\n")
out.append(arr("INV_OVERLAP_Q16", [int(round(65536 / o)) for o in overlap]))
out.append("// overlap * 10, for the page\n")
out.append(arr("OVERLAP10", [int(round(o * 10)) for o in overlap]))
# loudness held about even as grains pile up: 1 / sqrt(overlap) above 1, a little under 1 below
out.append("// the gain that keeps loudness about even as the overlap grows, Q15\n")
out.append(arr("OVERLAP_GAIN_Q15", [q15(0.9 / math.sqrt(max(1.0, o))) for o in overlap]))

# ---- pitch: 2^(n/12) for n 0..11 and 2^(k/768) for k 0..63, Q30
out.append("// 2^(n/12), Q30\n")
out.append(arr("SEMI_Q30", [int(round(2 ** (n / 12) * 2 ** 30)) for n in range(12)]))
out.append("// 2^(k/768): a 64th of a semitone, Q30\n")
out.append(arr("FINE_Q30", [int(round(2 ** (k / 768) * 2 ** 30)) for k in range(64)]))

# the step of C4 at 88,200 is 2^32 * 261.6256 / 88200; a sample at 44,100 played at its root moves 0.5 frames an output
# frame, so frames a frame (Q16) = step * (32768 / step_C4), the factor kept in Q40.
step_c4 = 2 ** 32 * 440.0 * 2 ** (-9 / 12) / OUT_RATE
inv = int(round(32768.0 / step_c4 * 2 ** 40))
assert inv < 2 ** 32
out.append(f"// 32768 / (the step of C4 at 88,200), Q40: a note's step times this, >> 40, is the source frames an output\n"
           f"// frame of a 44,100 sample whose root is C4 (Q16)\nstatic const unsigned INV_STEP_C4_Q40 = {inv}u;\n")

print("".join(out), end="")
