#!/usr/bin/env bash
#
# tools/flash.sh - build and flash the wave-1 firmware, optionally capturing
# the UART output afterwards.
#
#   tools/flash.sh                       build + flash
#   tools/flash.sh --capture             build + flash + capture until DONE
#   tools/flash.sh --capture --timeout 30 --expect "flash=4MB"
#
# The serial port defaults to /dev/cu.usbserial-110 and can be overridden with
# the PORT environment variable. Any argument other than --capture is forwarded
# verbatim to tools/capture.py.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Split off --capture; everything else goes to capture.py.
DO_CAPTURE=0
CAPTURE_ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--capture" ]; then
        DO_CAPTURE=1
    else
        CAPTURE_ARGS+=("$arg")
    fi
done

# export.sh is not written for `set -eu`; relax while sourcing it.
set +eu
# shellcheck source=tools/env.sh
source tools/env.sh
set -eu

PORT="${PORT:-/dev/cu.usbserial-110}"

echo "flash.sh: building and flashing on $PORT"
idf.py -p "$PORT" -b 460800 build flash

if [ "$DO_CAPTURE" -eq 1 ]; then
    echo "flash.sh: capturing UART output on $PORT"
    # bash 3.2: guard the array expansion so `set -u` tolerates zero extra args.
    python tools/capture.py --port "$PORT" ${CAPTURE_ARGS[@]+"${CAPTURE_ARGS[@]}"}
fi
