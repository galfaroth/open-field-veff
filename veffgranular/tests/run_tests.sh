#!/bin/sh
# veffgranular's host test: builds tests/host_test.cpp with the address and undefined-behaviour sanitizers and runs
# it. Rendered WAV files go to the folder given as the first argument (default: a fresh folder under /tmp).
# Usage: sh veffgranular/tests/run_tests.sh [out-folder]
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$(mktemp -d /tmp/veffgranular.XXXXXX)}
mkdir -p "$out"
c++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer "$here/host_test.cpp" -o "$out/host_test"
"$out/host_test" "$out"
echo "renders in $out"
