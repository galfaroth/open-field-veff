# Writes veffshaper_tables.h: python3.12 -I veff/veffshaper/make_tables.py > veff/veffshaper/veffshaper_tables.h
import math
Q31 = 2**31
def q(v, s=Q31): return max(-Q31, min(Q31 - 1, round(v * s)))
def arr(t, name, vals, per=8):
    out = [f"static const {t} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per): out.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    out.append("};"); return "\n".join(out)
def drive_db(d): return (d - 50) * 12 / 50 if d < 50 else (d - 50) * 24 / 50
print("// Made by veff/veffshaper/make_tables.py: do not edit by hand.")
print("// SINE: 1024 points + 1 of a turn, Q31. DRIVE_GAIN: the drive's gain in Q27 (-12 .. +24 dB). DRIVE_DB: rounded dB.")
print("// GATE_R: damp's 1/threshold / 512 in Q31 (threshold 0 .. 0.25 of full scale). PERIOD_X10: ripple cycles x 10.")
print("// OUT_GAIN: 0 .. -24 dB in Q31.")
print(arr("int", "SINE", [q(math.sin(2 * math.pi * i / 1024)) for i in range(1025)]))
print(arr("int", "DRIVE_GAIN", [q(10 ** (drive_db(d) / 20), 2**27) for d in range(101)]))
print(arr("signed char", "DRIVE_DB", [round(drive_db(d)) for d in range(101)], 20))
print(arr("int", "GATE_R", [0] + [q(1 / (0.25 * d / 100) / 512) for d in range(1, 101)]))
print(arr("short", "PERIOD_X10", [round((0.5 + 0.195 * p) * 10) for p in range(101)], 20))
print(arr("int", "OUT_GAIN", [q(10 ** (-a / 20)) for a in range(25)]))
