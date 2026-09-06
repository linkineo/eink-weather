#!/bin/sh
# Build and run the gfx host-side unit test.
#
# Compiles the vendored GUI_Paint.c, the ASCII fonts and gfx_text.c with the
# host compiler against a stub esp_log.h, then runs the assertions in
# host_test.c. No ESP-IDF and no hardware involved.
#
# Usage: test/run_host_test.sh [build-dir]
# Exits non-zero if the build fails or any assertion fails.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
COMP=$(dirname "$HERE")

CC=${CC:-cc}
BUILD=${1:-}
if [ -z "$BUILD" ]; then
    BUILD=$(mktemp -d "${TMPDIR:-/tmp}/gfx_host_test.XXXXXX")
    trap 'rm -rf "$BUILD"' EXIT INT TERM
else
    mkdir -p "$BUILD"
fi

BIN="$BUILD/host_test"

echo "gfx host test: compiling with $CC"

INCLUDES="-I $COMP/include -I $COMP/shims -I $HERE/stub_include"
COMMON="-std=gnu11 -O1 -g"

# Vendored Waveshare / STMicroelectronics sources: same warning suppressions the
# component CMakeLists applies on the ESP-IDF side, so the two builds agree on
# what is tolerated in third-party code.
VENDOR_W="-Wall -Wextra -Wno-type-limits -Wno-sign-compare"

# eink-weather sources: strict, warnings are errors.
OWN_W="-Wall -Wextra -Werror"

for src in GUI_Paint.c fonts/font8.c fonts/font12.c fonts/font16.c \
           fonts/font20.c fonts/font24.c; do
    obj="$BUILD/$(basename "$src" .c).o"
    # shellcheck disable=SC2086
    "$CC" $COMMON $VENDOR_W $INCLUDES -c "$COMP/$src" -o "$obj"
done

# shellcheck disable=SC2086
"$CC" $COMMON $OWN_W $INCLUDES -c "$COMP/gfx_text.c" -o "$BUILD/gfx_text.o"
# shellcheck disable=SC2086
"$CC" $COMMON $OWN_W $INCLUDES -c "$HERE/host_test.c" -o "$BUILD/host_test.o"

"$CC" "$BUILD"/*.o -lm -o "$BIN"

echo "gfx host test: running $BIN"
"$BIN"
