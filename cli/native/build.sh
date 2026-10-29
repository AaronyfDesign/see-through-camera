#!/bin/sh
# build.sh - build the native cardgen renderer (no external deps).
#   usage: sh cli/native/build.sh [output-dir]
# Produces: cardgen (macOS/Linux) or cardgen.exe (Windows via clang).
set -e
cd "$(dirname "$0")"

OUT="${1:-../native-build}"
mkdir -p "$OUT"

CC="${CC:-cc}"
SRC="cardgen.c"

# macOS
if command -v cc >/dev/null 2>&1 && [ "$(uname)" = Darwin ]; then
    cc -O2 -std=c11 -D_DEFAULT_SOURCE -o "$OUT/cardgen" "$SRC" -lm
    echo "built $OUT/cardgen"
    exit 0
fi

# generic unix
if command -v cc >/dev/null 2>&1; then
    cc -O2 -std=c11 -D_DEFAULT_SOURCE -o "$OUT/cardgen" "$SRC" -lm
    echo "built $OUT/cardgen"
    exit 0
fi

echo "no C compiler found (install Xcode CLT / clang)" >&2
exit 1
