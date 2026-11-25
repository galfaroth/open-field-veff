# Findings: the drum sampler's kit and its 24 slices

OS 1.7.3+0 (os.ldr SHA-256 d134eb1a…d0ec). Read in the disassembly on a computer; **nothing tried on a unit**. The
probe in this folder exists to confirm or refute it. Every address belongs to 1.7.3+0 only.

## 1. Holders, objects, pool (confidence high)

- Holder table `0x0802f0ac`: 8 words, one per sound slot, filled once by `0x8f014808` (allocates 4 bytes per slot,
  constructor `0x8f013700(holder, slot)`). Accessor `0x8f014848(slot)` returns `[0x0802f0ac + 4·slot]`, asserts
  slot < 8 (DrumSampler.cpp:337).
- holder+0 is a 0xd774-byte sample object, built by the same constructor as the synth's, `0x8f00302e(obj, 3, slot)`
  (kind 3 = drum). Same layout: +0 data, +0xd768 frames, +0xd76c words, byte +0xd770 stereo, byte +0xd771 kind.
- Drum pool (from veffsampleprobe's FINDINGS): `0x8d518598 + slot·0x35d540` (= 882000 stereo frames, 20 s). Capacity
  helper `0x8f0137c0(holder)` returns 0x81330 frames (mono) or 0xd7550 (stereo)... read: stereo flag selects
  0xd7550, else 0x81330 (confidence medium on which is which).
- Kit sample load: `0x8d4ef9b0(slot, path, flag)` → `0x8f013754` → the shared file load `0x8f003434`. Overview
  rebuild: `0x8d4eb7b8(slot)` → `0x8f0037be`.

## 2. The drum voice and its parameter block (confidence high for the format, medium for field roles)

- Drum voice render `0x8f0138c0(voice, out, params)`, vtable word `0x848cb730` (class at `0x848cb728`).
- Drum voices: `0x119062d8 + 0x38·i` (accessor `0x8f0148ac`, i < 8; another accessor `0x8f01c23c` uses stride 0x48 —
  one of the two is the d-box; unverified).
- The per-note parameter block (6 words) the render reads:
  - `+0` pitch, packed: semitones in the top 7 bits (±48), fine part below (edit `0x8f01c500`);
  - `+4` start and `+8` end: **Q31 fractions of the object's capacity** (not frames, not ×4058). The render multiplies
    each by (capacity << 11) as a fractional multiply, giving a 20.11 frame index; asserts start ≥ 0, end ≥ 0
    (DrumSampler.cpp:160–161). Minimum length enforced by edit: 0x00100000 / constant `0x848cc7e0`.
  - `+0xc` top 3 bits: a mode compared to 3 (with the voice's `+0x2c`), likely play mode;
  - `+0x10` low 2 bits: direction/mode (0..2), top 2 bits read as a further flag (knob 4 edit `0x8f01c5a8`);
  - `+0x14` volume (bit 0 a flag) (knob 5 edit `0x8f01c5d0`).
- The kit file's "start"/"end" (×4058 a frame in the AIFF JSON) are therefore converted on load to Q31 of capacity;
  the converter was not located.

## 3. Where the 24 slices live (confidence LOW — the probe decides)

- Knob edits go through `0x8f01c41c(knob, delta, kind, …, block)` (drum_sampler_param.cpp:72; knob <6, kind 9 drum /
  10 d-box), jump table `0x848cc98c`. Start = `0x8f01c370` writes block+4, end = `0x8f01c3c8` writes block+8; with
  the fine flag the step is relative to the current length (`0x8f01c30a`), else to frames·const (`0x8f01c330`, which
  reads the current drum slot from `0x11904708`).
- All seven callers (UI/MIDI/LFO, 0x8c0b770c, 0x8d50b3aa, 0x8d50bd7e, 0x8d50d016, 0x8d50d11c, 0x8d50d6a8,
  0x8d50e402) pass the **same block `0x11904754` in L1**. The current drum slot is `[0x84a01528]` (getter
  `0x8d504224`); per-slot words at `0x11904734 + 4·slot` and `0x11904714 + 4·slot` are passed alongside (offsets
  into a table at `0x840adf74`).
- So `0x11904754` is the edit block of **the key being edited**; the array holding all 24 keys was not found. Best
  guess: it sits in the per-slot DSP patch reached through those L1 tables, or is copied into `0x11904754` on key
  select. The probe dumps 0x11904700..0x11904800 and all drum voices so one unit session shows which.
- Kit JSON level (db2/BgFile.cpp, high): the drum patch is kept as a JSON tree per slot at `0x0802377c + slot·0x34`
  (+8 the json value). `0x8d4eb7d8` writes keys pitch/start/end/playmode/volume/reverse into it (save path);
  `0x8d4ebc68…` copies them from a loaded json patch, then reloads the sample with `0x8d4ef9b0` (keys at
  `0x8401e29c/2a4/2a8/2b4/2bc`). The json helpers are `0x8c085e84` (get member) and `0x8c0845b8` (set/replace).

## 4. Setting slices from a pack (not ready)

- No stock "set slice n" function was found. Writing block+4/+8 of `0x11904754` changes only the edited key.
  Candidate route for a chop pack: write all 24 into the JSON patch (keys start/end, ×4058) and call the patch reload
  path (`0x8d4ebc68` area → `0x8d4ef9b0`), on the UI thread. Unverified; needs the converter and its caller first.
- The drop: `0x8f00bac0` (drum twin, holder via `0x8f014848`) → `0x8f004204` renders into the object in place.
  Not re-read here.

## Still unknown
- The 24-key store and the JSON→Q31 converter. Which voice stride (0x38 or 0x48) is the drum's.
