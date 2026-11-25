#!/bin/sh
# Builds veffgranular.pack from this repository into its root, inside a temporary root of links to an open-field 0.14
# checkout (nothing is written into the checkout), and REFUSES a pack that needs a name which is not open-field's:
#   OPEN_FIELD=<the open-field checkout> sh veffgranular/tests/build_pack.sh
# The unit has no C library. A struct assigned or initialised as a whole, or a division, can make the compiler call
# memcpy, memset or a libgcc routine; the loader then leaves the pack out ("nobody offers the name it needs").
# tools/pack_build.py check prints such a name in its line "wants:" and does not fail, so this script looks twice:
# at the compiler's assembly (tests/needs.py) and at that line.
set -e
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin:$PATH
WT="$(cd "$(dirname "$0")/../.." && pwd)"
OF="${OPEN_FIELD:?set OPEN_FIELD to an open-field 0.14 checkout}"; OF="$(cd "$OF" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
for d in tools module profiles linker open-xy; do ln -s "$OF/$d" "$W/$d"; done
mkdir -p "$W/veff" "$W/out" "$W/build"; ln -s "$WT/veffgranular" "$W/veff/veffgranular"
cd "$W"
python3 -I tools/pack_build.py card --recipe veff/veffgranular/recipe.json --out veff/veffgranular/veffgranular_card.s
python3 -I tools/pack_build.py build3 --recipe veff/veffgranular/recipe.json --profile profiles/op1f-1.7.3.json --out "$W/out/veffgranular.pack" --build-dir "$W/build" | tail -1
python3 -I veff/veffgranular/tests/needs.py "$W/build/veffgranular.cpp.s"
python3 -I tools/pack_build.py check --pack "$W/out/veffgranular.pack" > "$W/check.txt"
grep '^wants:' "$W/check.txt"
if grep '^wants:' "$W/check.txt" | sed 's/^wants: *//' | tr ',' '\n' | sed 's/^ *//' | grep -v '^op1f_[a-z0-9_]*$' | grep -q .; then
  echo "REFUSED: the pack wants a name that is not open-field's"; exit 1
fi
cp "$W/out/veffgranular.pack" "$WT/veffgranular.pack"
shasum -a 256 "$WT/veffgranular.pack"; stat -f %z "$WT/veffgranular.pack"
