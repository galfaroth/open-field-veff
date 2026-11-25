# Writes graindelay_tables.h: python3 -I veff/veffgraindelay/make_tables.py > veff/veffgraindelay/graindelay_tables.h
import math
FS = 44100.0; FREQS = 64; F_LOW = 1.0; F_HIGH = 150.0
hz = [F_LOW * (F_HIGH / F_LOW) ** (i / (FREQS - 1)) for i in range(FREQS)]
print("// Made by make_tables.py: do not edit by hand.")
print("enum { FREQS = %d };" % FREQS)
# A grain lasts two launch periods (two grains overlap): its window's phase runs 2^32 over 2 * FS / f frames.
print("static const unsigned FREQ_INC[FREQS] = {" + ",".join(str(int(round(f * 2**31 / FS))) for f in hz) + "};")
print("static const unsigned FREQ_LEN[FREQS] = {" + ",".join(str(int(math.ceil(2 * FS / f))) for f in hz) + "};")
print("static const unsigned short FREQ_TENTHS[FREQS] = {" + ",".join(str(int(round(f * 10))) for f in hz) + "};")
# 2^(16 k / 1200), Q15: one octave in steps of 16 cents (76 entries, the last 2.0)
print("static const int EXP2_16C[76] = {" + ",".join(str(int(round(2 ** (16 * k / 1200) * 32768))) for k in range(76)) + "};")
