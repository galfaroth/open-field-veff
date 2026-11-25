#!/bin/sh
# Builds veffchop.pack into this repository's root, inside a temporary root of links to an open-field 0.12 checkout
# (nothing is written into the checkout), and REFUSES a pack that needs a name which is not open-field's:
#   OPEN_FIELD=<the open-field 0.12 checkout> sh veffchop/tests/build_pack.sh
# The unit has no C library: a memcpy, memset or libgcc routine the compiler made up leaves the pack out at the start.
# tests/needs.py looks at the compiler's assembly, and the 'wants:' line of pack_build.py check is looked at too.
set -e
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin:$PATH
WT="$(cd "$(dirname "$0")/../.." && pwd)"
OF="${OPEN_FIELD:?set OPEN_FIELD to an open-field 0.12 checkout}"; OF="$(cd "$OF" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
for d in tools module profiles linker open-xy; do ln -s "$OF/$d" "$W/$d"; done
mkdir -p "$W/veff" "$W/out" "$W/build"; ln -s "$WT/veffchop" "$W/veff/veffchop"
cd "$W"
python3 -I tools/pack_build.py card --recipe veff/veffchop/recipe.json --out veff/veffchop/veffchop_card.s
python3 -I tools/pack_build.py build3 --recipe veff/veffchop/recipe.json --profile profiles/op1f-1.7.3.json --out "$W/out/veffchop.pack" --build-dir "$W/build"
python3 -I veff/veffchop/tests/needs.py "$W/build/veffchop.cpp.s"
python3 -I tools/pack_build.py check --pack "$W/out/veffchop.pack" > "$W/check.txt"
grep '^wants:' "$W/check.txt"
if grep '^wants:' "$W/check.txt" | sed 's/^wants: *//' | tr ',' '\n' | sed 's/^ *//' | grep -v '^op1f_[a-z0-9_]*$' | grep -q .; then
  echo "REFUSED: the pack wants a name that is not open-field's"; exit 1
fi
cp "$W/out/veffchop.pack" "$WT/veffchop.pack"
shasum -a 256 "$WT/veffchop.pack"; stat -f %z "$WT/veffchop.pack"
