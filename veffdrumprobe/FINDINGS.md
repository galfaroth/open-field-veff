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

## 5. After probe v1 on the unit, and a second reading (supersedes section 3)

**What v1 showed.** Slot 1: holder `0x8c0fe110`, object `0x8c195c98`, data at the pool `0x8d518598`, frames 882000,
stereo 1. Words were 1719912 for the first kit and 1026000 for the second. Kind is **0**, not 3: the constructor's
second argument is not stored as the kind byte, or something writes the byte later. The edit block at `0x11904754`
changed twice and once held eight equal words `1f400000`. The voices at the 0x38 spacing have the drum voice
class word `0x848cb728`.

**The 24 keys live in a table (confidence high).**
- The table is at `0x84a03cc8 + slot·0x300 + key·0x20`: 24 records of 8 words for each slot.
- The slot is the current drum slot, `[0x84a01528]`.
- The audio loop (`0x11a03728`..`0x11a039a6`) runs once for each of the 8 drum voices. For each voice it copies the
  record of that voice's key into the scratch block `0x11904754`, then calls the voice's render (vtable +8) with that
  block.
- The record's index comes from `0x11903560 + 4·voice`. The UI-side helpers (`0x8d5056a0`, `0x8d505720`,
  `0x8d5057c0`) compute the same record address and pass it to the voice's vtable +0xc / +0x14 / +0x18.
- So `0x11904754` is only a scratch copy, shared with another path (the 8 equal words). Nothing is stored there.
- The function at `0x8d4f4840`..`0x8d4f4c00` walks the same table for one slot (×0x300, 24 keys, 0x20 apart). It reads
  the words and builds JSON values under the kit's key names: the table-to-JSON direction (save). The functions near
  `0x8d4f5236`..`0x8d4f55de` also name the table. The JSON-to-table direction is most likely there; not read
  (medium).

**Inside a key record** (as in section 2; medium until v2): +0 pitch, +4 start, +8 end (Q31 of capacity), +0xc
top 3 bits a mode, +0x10 mode or direction bits, +0x14 volume, +0x18 and +0x1c not known.

**The per-voice words are not start and end (high).** voice+8..+0x14 are the four gain words that `0x8f0137ec`
writes, a pan law: `7ffffffe 0 7ffffffe 0` and `41d0cda0 6dc86b62 …` are two pan positions. voice+0x18 is the play
position, which moves. The key's start and end are not copied into the voice; the render reads them from the scratch
block every block.

**The L1 bytes at `0x11904700`** (0 2 0 0 0 c c 7 d 7 7 7 …) were not traced. They are not start or end. They could
be per-key small settings, such as groups.

**Can a chop pack set 24 slices?** Probably: write +4 and +8 of the 24 records at
`0x84a03cc8 + [0x84a01528]·0x300`, on the UI thread (confidence medium-high).
- No refresh call should be needed: the audio loop copies a record into the scratch block on every block, so a write
  takes effect at the next block.
- Write start and end with two aligned word stores. The worst case is one block with the new start and the old end.
- Saving the kit should carry the change, because the save builds the JSON from this table (section 5's walker).
  Medium: the path from "save" to that function was not traced.
- The JSON tree plus reload `0x8d4ebc68` also reloads the sample, so it is the heavier route.

v2 of the probe logs each changed key record of the current slot (`key S.K` plus 8 words) and the record index of
each voice (`voice keys`).

## 6. After probe v2 on the unit: the key record, settled

The table at `0x84a03cc8 + slot·0x300 + key·0x20` is confirmed on the unit. Kits were loaded into slot 1 (current
slot 0); a load rewrites all 24 records about 0.5 s after the slot's `words` changes. `voice keys` followed the keys
played (index 4 = key 5, 12 = key 13). The owner turned start, end, pitch, mode and volume on key 5; each turn moved
exactly one word:

| Word | Knob | Meaning and scale | Confidence |
| --- | --- | --- | --- |
| +0 | pitch | Signed semitones in the top 7 bits (`>>25`; `0x18000000` = +12, `0x1a000000` = +13, `ff21…` = −1 plus a fine part); the low 25 bits are the fine part (a step of `0x52bf5`). 0 = no transposition. | high |
| +4 | start | Q31 fraction of the capacity (below) | high |
| +8 | end | Q31 fraction of the capacity. Key n's end is about key n+1's start minus one unit (`0x0000962d`, one frame). | high |
| +0xc | mode | Play mode in the top 3 bits: `0x10…` = 0, `0x30…` = 1 (the default), `0x50…` = 2, `0x70…` = 3. The lower bits are the knob's position between steps (`43fb0000` was seen during the turn). The render's mode 3 also reads voice+0x2c. Which name goes with which number (one-shot, gate, loop …) was not tested. | high for the encoding, low for the names |
| +0x10 | (not turned) | `0x20000000` on every key of every kit; the render takes its low 2 bits and top 2 bits. Most likely direction/reverse with "forward" = `0x20000000`. No reversed key was seen. | low |
| +0x14 | (not turned) | **Pan**, not volume. It feeds the pan law `0x8f0137ec`. `0x40000000` = centre, `0x00000001` / `0x7ffe0001` = hard left/right. Bit 0 selects the second pan law (a kit-wide setting: some kits have it on every key). | medium-high |
| +0x18 | — | 0 everywhere | — |
| +0x1c | volume | `0x20000000` is the default (likely unity, 1.0 in Q29); kits carry per-key values from `0x15c6…` to `0x3ae5…`. | high for the knob, medium for unity |

Section 2's guesses for +0x10 and +0x14 are superseded by this table.

**What the fractions are fractions of (high for stereo).**
- The render multiplies each fraction by the capacity from `0x8f0137c0`: 882000 frames when the stereo byte is set,
  529200 when it is clear (mono not seen on the unit; medium).
- Check against the slot's `words` (= 2·frames + 8 for stereo): the four stereo kits had 810099, 859953, 750870 and
  869581 frames. Key 24's end × 882000 / 2^31 gives 810082, 859932, 750849 and 869566.
- So key 24 ends about 17–21 frames before the last frame written, and **frame f = fraction × 882000 / 2^31**.
- In the other direction: fraction = f × 2^31 / 882000, about 2434.8 units a frame. Compute it as
  `(f << 31) / 882000` in 64 bits, or as `f × 2434 + (f × 0.8…)`; a 32-bit `f × 2435` overflows past 881k frames.
- The kit file's ×4058 scale is the file format only; it never appears in memory.

**`0x11904700..` (v1 `w` lines).** Not traced further. They are not start, end, pitch, mode or volume.

## 7. Design: the chop pack `veffchop` (no code yet)

**Aim.** Chop the current drum slot's sample into 24 slices — equal, lazy (onsets with a minimum gap) or transient —
and write them as the kit's 24 start/end points, the way OP-XY does.

**What it writes, and nothing else.** For the current slot `s = [0x84a01528]` (0..7), and for keys k = 0..23:
- +4 (start) and +8 (end) of `0x84a03cc8 + s·0x300 + k·0x20`, as Q31 fractions of the capacity (`f·2^31/882000`
  stereo, `/529200` mono).
- Never pitch, mode, pan or volume: the kit keeps its other settings.
- No firmware code changes (0 stock changes).

**Where the frames come from.** Read only, through the chain confirmed here:
- object = `[[0x0802f0ac + 4s]]`, data = `[object]`, `words = [object+0xd76c]`, stereo = byte `[object+0xd770]`;
- the sample's own length is `(words − 8) / 2` frames for stereo and `words − 8` for mono (mono unverified);
- int16 data from data+16 (the head pad).
- Equal mode needs only the length. Lazy and transient need an energy envelope over the data; read it in bounded
  pieces (for example 4096 frames a frame tick) so one frame never takes long.

**When and on which thread.**
- On the display thread (the base's frame function), when the owner asks: a key combination or the pack's own page
  (level 2+ if a page is wanted; level 1 can only react to existing state).
- Analysis spreads over frames; the 48 words are written in one frame.
- The audio thread re-copies each record into its scratch block every block, so no refresh call is needed. A voice
  already sounding keeps its old points until the next block, then reads the new ones.
- Write start before end only when the new start is lower than the old end, otherwise end first, so a block that
  reads half an update never sees start > end. The render asserts both ≥ 0, and the stock edit keeps a minimum length
  of `0x00100000`.

**Guards.**
- Slot index < 8.
- Every address range-checked as in the probe: table inside DDR, object of 0xd774 bytes, frames ≤ capacity.
- Refuse when `words` is 0 or below a minimum (no sample), or the stereo byte is not 0/1.
- Refuse while a load is in flight: `words` must be unchanged over 2 checks about 0.5 s apart (the probe saw the
  records rewritten ~0.5 s after `words` changed). Re-read `words` just before writing.
- Clamp every fraction to [0, 0x7fffffff] and keep end − start ≥ `0x00100000`.
- Write only when the owner asks, never on its own, so a kit load is never overwritten silently.

**Saving the kit.**
- The table-to-JSON walker (`0x8d4f4840`..`0x8d4f4c00`) builds the kit's JSON from these records, so a stock save of
  the kit (or the stock save at power-off/sound change) should carry the new slices.
- Medium: the path from "save" to the walker has not been traced. Test on the unit: chop, save as a new kit, power
  cycle, reload it, and compare the `key` lines.
- If the save does not carry them, the fallback is the JSON tree at `0x0802377c + s·0x34` plus a stock save, not the
  reload `0x8d4ebc68`: that reloads the sample too.

**Not in v1 of the pack.** Mono kits until a mono kit is seen with the probe; the d-box kind; drop-then-chop as one
action (the drop's own writes to the records were not traced).
