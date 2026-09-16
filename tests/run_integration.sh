#!/bin/sh
# tests/run_integration.sh - end-to-end smoke test of the raytracer CLI.
#
# Builds the project if needed, renders a 160x120 BMP, then validates the
# file size and BMP magic bytes independently of the C test suite.
#
# POSIX sh, `set -eu`. Exits 0 on success, 1 on failure.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/tmp}
OUT="$TMPDIR/rt_it.bmp"
EXPECTED_SIZE=57654   # 54 + ((160*3+3)/4)*4 * 120 = 54 + 480*120

# --- build if the binary is missing ---------------------------------------
if [ ! -x "$ROOT/raytracer" ]; then
    echo "integration: building project (./raytracer missing)..."
    ( cd "$ROOT" && make )
fi

# --- render ---------------------------------------------------------------
rm -f "$OUT"
"$ROOT/raytracer" \
    --width 160 --height 120 --samples 4 --depth 5 --seed 42 \
    --out "$OUT"

# --- verify the file exists and is non-empty ------------------------------
if [ ! -f "$OUT" ]; then
    echo "integration: FAIL: output file '$OUT' was not created" >&2
    exit 1
fi

size=$(wc -c < "$OUT" | tr -d ' ')
if [ "$size" -le 0 ]; then
    echo "integration: FAIL: output file '$OUT' is empty" >&2
    exit 1
fi

# --- verify exact size ----------------------------------------------------
if [ "$size" -ne "$EXPECTED_SIZE" ]; then
    echo "integration: FAIL: expected $EXPECTED_SIZE bytes, got $size" >&2
    exit 1
fi

# --- verify magic bytes ---------------------------------------------------
magic=$(head -c 2 "$OUT")
if [ "$magic" != "BM" ]; then
    echo "integration: FAIL: bad BMP magic bytes (got '$magic', expected 'BM')" >&2
    exit 1
fi

echo "integration: OK"
exit 0
