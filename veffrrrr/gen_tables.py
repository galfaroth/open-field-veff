# Writes veffrrrr_tables.h: python3.12 -I veff/veffrrrr/gen_tables.py > veff/veffrrrr/veffrrrr_tables.h
import math
FS = 44100.0
Q = 2**31
def q(v): return max(-Q, min(Q - 1, int(round(v * Q))))
def arr(name, vals, per=8, typ="int"):
    s = f"static const {typ} {name}[{len(vals)}] = {{\n"
    for i in range(0, len(vals), per): s += "    " + ", ".join(str(v) for v in vals[i:i+per]) + ",\n"
    return s + "};\n"
FREQS = 64
hz = [30.0 * (18000.0 / 30.0) ** (i / (FREQS - 1)) for i in range(FREQS)]
RES = [0.0, 0.3, 0.5, 0.65, 0.78, 0.87, 0.93, 0.97]
out = ["// Made by gen_tables.py in this folder: do not edit by hand.\n"]
out.append(arr("FREQ_HZ", [int(round(h)) for h in hz]))
out.append(arr("DRIVE_Q", [q(10 ** (d / 20) / 64) for d in range(37)]))          # 0..36 dB, gain/64
out.append(arr("AMT_G", [q(2 ** (a / 25) / 16) for a in range(101)]))            # shaper pre-gain 1..16, /16
out.append(arr("OUT_Q", [q(10 ** ((d - 24) / 20) / 4) for d in range(37)]))     # -24..+12 dB, gain/4
out.append(arr("K_HALF", [q((2 - 2 * r) / 2) for r in RES]))                    # SVF damping k/2
svf = []
for h in hz:
    g = math.tan(math.pi * min(h, 0.45 * FS) / FS)
    for r in RES:
        k = 2 - 2 * r
        a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2
        svf += [q(a1), q(a2), q(a3)]
out.append(arr("SVF", svf, 9))                                                    # [freq][res][a1,a2,a3]
out.append(arr("COMB_LEN", [max(2, min(1023, int(round(FS / h)))) for h in hz]))
out.append(arr("COMB_FB", [q(0.2 + 0.7 * r / 0.97) for r in RES]))               # 0.2 .. 0.9
out.append(arr("ONEPOLE", [q(1 - math.exp(-2 * math.pi * h / FS)) for h in hz]))
out.append(arr("SINE", [q(math.sin(2 * math.pi * i / 1024)) for i in range(1025)]))
print("".join(out), end="")
