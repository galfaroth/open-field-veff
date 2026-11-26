# veffchop 6: mono samples, and a drop on the page

OS 1.7.3+0. **Version 6 has not run on a unit.**

## Mono

The drum sampler's render takes its capacity from the object's stereo byte (`0x8f0137c0`): 882,000 frames when it is
set, 529,200 when it is 0. A key's start and end are fractions (Q31) of that capacity. A mono sample is a halfword a
frame; its `words` are its frames and 8 (a stereo one's twice its frames and 8). Version 6 reads both, draws both, and
writes a mono kit's points as fractions of 529,200 (`q31_of_frame(f, 529200)`, exact for every frame, tested against
64-bit arithmetic).

## The drop

**Why nothing happened on v4's page.** The drop key goes to the screen shown; on veffchop's page that is the base's
lent screen, which has no drop.

**What v6 does.** It uses the base's sample helpers (docs/sample.md in open-field-catalog, from base 40 for a drum
place, commit 3914b2f on `drum-page`): on the drop key's down (no shift) the key is taken; on its up the input
function asks `op1f_sample_drop(card, 0, OP1F_PLACE_DRUM + slot)` (ask 0, veffchop's drum engine) and stores the answer;
the frame function reads `op1f_sample_get` for that place until `done` is the ticket. The unit does the drum sampler
page's own drop in its own turn (the base's DD at the drum drop job). Nothing of the firmware's is called by the pack.
Shift and drop stays the unit's.

**After the drop** the base's `dropped` is the sample's length (the firmware's `words` still say the sample loaded
before), until a sound is loaded there. veffchop analyses the sample again and shows `N SLICES`; it applies nothing by
itself: press the fourth knob to write the points. The stock drop does not change the 24 keys' points.

The sample's address and channels come from `op1f_sample_get` too; the edited slot, the object's `words` and the key
records are still read by veffchop at the addresses of FINDINGS.md.

## The log

- `veffchop: drop asked into drum <n>: ticket <t>`
- the base's `sample: drop <t> into drum <n> for 'veffchop': queued` and `sample: drop <t>: <frames> frames rendered (stereo <s>, room <r>) after <k> frame(s)`
- `veffchop: drop <t> into drum <n> done: answer <a>, dropped <frames> frames, stereo <s>, after <ms> ms; <d> done <r> refused`
- then `veffchop: slot <n>: <frames> frames, mono|stereo, a drop; analysing ...` and `veffchop: slot <n> analysed in ...`
- refused at once: `veffchop: drop into drum <n> refused at once: <answer> (<words>)` and the base's `sample: 'veffchop' asked a drop into drum <n> (ask 0): ...`; the page says `NOTHING LIFTED` or `DROP REFUSED -<n>`.
