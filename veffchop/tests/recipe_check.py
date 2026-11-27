#!/usr/bin/env python3
"""veffchop 4's recipe and pack against an open-field-catalog checkout at base 43 or later (open-field 0.25, the drum engine).

  python3 -I veffchop/tests/recipe_check.py <the checkout> <veffchop/recipe.json> [<the pack's `check` text>]

The recipe: one ask, a drum engine named 'veffchop' with its shown function, read by the checkout's own pack_build as
the base reads it; no drum's page of the form before (kind 'drum page', mark op1f_drum_page_1). With the text of
`pack_build.py check` for the built pack: the pack needs op1f_drum_engine_1 and no name but open-field's. Answers 0 when
all holds, 1 with what does not.
"""
import importlib.util
import json
import os
import re
import sys


def main(argv):
    checkout, recipe_path = argv[1], argv[2]
    spec = importlib.util.spec_from_file_location("pack_build", os.path.join(checkout, "tools", "pack_build.py"))
    pb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pb)
    with open(recipe_path, encoding="utf-8") as f:
        recipe = json.load(f)
    faults = []
    if recipe.get("version") not in (4, 5, 6, 7, 8, 9, 10):
        faults.append("the recipe's version is %r, not 4" % recipe.get("version"))
    asks = recipe["card"]["asks"]
    if len(asks) not in (1, 2) or (len(asks) == 2 and asks[1].get("kind") != "memory"):
        faults.append("the card has %d asks: the drum engine first, then (v8) the memory of the copy" % len(asks))
    try:
        ask = pb.drum_page_of_card(1, asks[0], [])
        if ask != (12, "veffchop", 0, {"shown": "veffchop_shown"}, {}):
            faults.append("the ask reads as %r" % (ask,))
    except ValueError as e:
        faults.append("the checkout's pack_build refuses the ask: %s" % e)
    if pb.KIND_WORDS_6.get(12) != "drum engine" or pb.DRUM_PAGE_MARK != "op1f_drum_engine_1":
        faults.append("the checkout is not at base 43's drum engine")
    text = json.dumps(recipe)
    if "drum page" in text or "op1f_drum_page_1" in text:
        faults.append("the recipe still names the drum's page of the form before")
    if len(argv) > 3:
        with open(argv[3], encoding="utf-8") as f:
            check = f.read()
        wants = re.findall(r"^wants:(.*)$", check, re.M)
        names = [n.strip() for line in wants for n in line.split(",") if n.strip()]
        if "op1f_drum_engine_1" not in names:
            faults.append("the pack does not need op1f_drum_engine_1 (wants: %s)" % ", ".join(names))
        if "op1f_drum_page_1" in names:
            faults.append("the pack still needs op1f_drum_page_1")
        other = [n for n in names if not re.fullmatch(r"op1f_[a-z0-9_]*", n)]
        if other:
            faults.append("the pack wants names that are not open-field's: %s" % ", ".join(other))
    for fault in faults:
        print("FAULT: " + fault)
    print("recipe check: %s" % ("in order" if not faults else "%d fault(s)" % len(faults)))
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
