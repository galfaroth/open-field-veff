# Prints the outside names a compiled source needs, and fails when one of them is not an open-field name.
#   python3 -I needs.py <the compiler's assembly output of veffgranular.cpp>
# A pack may need only names the base offers (op1f_*, its own card). The unit has no C library: a call of memcpy,
# memset or a libgcc routine (___divsi3 ...) that the compiler made up leaves the pack out at its start
# ("nobody offers the name it needs"), and tools/pack_build.py check does not notice it.
import re, sys
text = open(sys.argv[1]).read()
defined = set(re.findall(r'^([A-Za-z_.$][\w.$]*):', text, re.M))
defined |= set(re.findall(r'^\s*\.(?:comm|lcomm|local)\s+([A-Za-z_.$][\w.$]*)', text, re.M))   # statics kept in zeroed memory
used = set()
for pattern in (r'\b(?:call|jump(?:\.[sl])?)\s+([A-Za-z_][\w.$]*)\s*;', r'=\s*([A-Za-z_][\w.$]*)\s*;', r'\.(?:long|4byte|picptr)\s+([A-Za-z_][\w.$]*)'):
    used |= set(re.findall(pattern, text))
registers = re.compile(r'(?:[RP][0-7]|SP|FP|CC|A[01]|[ILBM][0-3]|LC[01]|LT[01]|LB[01]|ASTAT|RETS|R[0-7]\.[LHB])$', re.I)
used = set(u for u in used if not registers.match(u))
outside = sorted(u for u in used if u not in defined)
bad = [u for u in outside if not re.fullmatch(r'_?op1f_[a-z0-9_]+', u)]
print('needs: ' + ' '.join(outside))
if bad:
    print('NOT OPEN-FIELD NAMES: ' + ' '.join(bad))
    sys.exit(1)
