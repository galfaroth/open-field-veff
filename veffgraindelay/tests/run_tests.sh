#!/bin/sh
# Builds and runs veffgraindelay's host test under the address and undefined-behaviour sanitizers.
#   OPEN_FIELD  an open-field checkout, for module/build_r6e_card.h (default: the checkout this folder sits in, as
#               veff/veffgraindelay/tests)
#   first argument: the folder for the WAV files and the page's pictures (default: ./renders, made if missing)
set -e
here=$(cd "$(dirname "$0")" && pwd)
of=${OPEN_FIELD:-$here/../../..}
out=${1:-./renders}
mkdir -p "$out"
bin="$out/host_test"
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DOP1F_HOST '-Dstatic_assert(...)=' -I"$of/module" -I"$here/.." -o "$bin" "$here/host_test.cpp"
"$bin" "$out"
# the page as PNG beside the PPM (python's zlib only)
python3 -I - "$out/page.ppm" "$out/page-shift.ppm" <<'EOF'
import sys, zlib, struct
for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split()); px = parts[3]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path[:-4] + '.png', 'wb').write(png)
EOF
