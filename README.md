# Veff packs for open-field

Effects for the OP-1 Field, as packs for [open-field](https://github.com/LaP0573/open-field)
(OS 1.7.3, base 30, level 10). Each effect appears in the unit's own lists of effects
(shift and T3 on a sound, the master's list on the mixer) and on every wagon of fx train.
Not affiliated with Teenage Engineering.

| Pack | What it is | File |
| --- | --- | --- |
| veffbode | A Bode frequency shifter inside a delay's feedback loop. Knobs: shift, phase, feedback, mix; with shift held: delay (note or ms), sideband, tone, smear, cross. Starts as a jet flanger. Stands on `delay`. | [`veffbode.pack`](veffbode.pack) |

More Veff effects (veffrosion, veffsaturator, veffshaper, veffgrrr) are on the way.

## Install

Pick it on the open-field install page, or copy `veffbode.pack` into the unit's `packs` folder beside `base.pack`.

## Build

The sources in `veffbode/` build inside an open-field 0.12 checkout, placed at `veff/veffbode/`:

```bash
python3 -I tools/pack_build.py build3 --recipe veff/veffbode/recipe.json --profile profiles/op1f-1.7.3.json --out veffbode.pack
python3 -I tools/pack_build.py check --pack veffbode.pack
```

`face.py` writes the page's picture, `veffbode.svg.make`.
