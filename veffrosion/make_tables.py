# Writes erosion_tables.h: python3.12 -I veff/veffrosion/make_tables.py > veff/veffrosion/erosion_tables.h
import math
FS = 44100.0; STEPS = 128; WIDTHS = 11; TARGET = 0.35; NOISE_RMS = (2**31 >> 4) / math.sqrt(3) / 2**31
hz = [30.0 * 600.0 ** (i / (STEPS - 1)) for i in range(STEPS)]
q31 = lambda x: max(-2**31, min(2**31 - 1, int(round(x * 2**31))))
print("// Made by make_tables.py: do not edit by hand.")
print("enum { FREQ_STEPS = %d, WIDTH_LEVELS = %d };" % (STEPS, WIDTHS))
print("static const int SINE[1025] = {" + ",".join(str(q31(math.sin(2*math.pi*i/1024))) for i in range(1025)) + "};")
print("static const unsigned FREQ_INC[FREQ_STEPS] = {" + ",".join(str(int(round(f * 2**32 / FS))) for f in hz) + "};")
print("static const unsigned short FREQ_HZ[FREQ_STEPS] = {" + ",".join(str(int(round(f))) for f in hz) + "};")
rows = []
for i, f in enumerate(hz):
    g = math.tan(math.pi * f / FS)
    for w in range(WIDTHS):
        k = 0.1 * 40.0 ** (w / (WIDTHS - 1))
        a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2
        # energy of the normalized band-pass (k * v1) impulse response
        ic1 = ic2 = 0.0; e = 0.0
        for n in range(200000):
            v0 = 1.0 if n == 0 else 0.0
            v3 = v0 - ic2; v1 = a1 * ic1 + a2 * v3; v2 = ic2 + a3 * v3
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2
            e += (k * v1) ** 2
            if n > 2000 and abs(v1) + abs(v2) < 1e-12: break
        make = TARGET / (NOISE_RMS * math.sqrt(e)) * k      # applied to v1
        sh = 0
        while make >= 1.0: make /= 2; sh += 1
        rows.append("{%d,%d,%d,%d,%d}" % (q31(a1), q31(a2), q31(a3), q31(make), sh))
print("struct Svf { int a1, a2, a3, make; int shift; };")
print("static const Svf SVF[FREQ_STEPS * WIDTH_LEVELS] = {" + ",\n".join(rows) + "};")
