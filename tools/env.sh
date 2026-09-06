# tools/env.sh - activate the ESP-IDF toolchain for this project.
#
# Usage (zsh or bash):   source tools/env.sh
#
# This script must be SOURCED, not executed, because it exports IDF_PATH and
# extends PATH in the calling shell. It never writes to the IDF clone.

EINK_IDF_PATH="/Users/2pat/devl/esp-idf"

if [ "${IDF_PATH:-}" = "$EINK_IDF_PATH" ] && command -v idf.py >/dev/null 2>&1; then
    # Already activated in this shell - nothing to do.
    :
else
    if [ ! -f "$EINK_IDF_PATH/export.sh" ]; then
        echo "tools/env.sh: ESP-IDF not found at $EINK_IDF_PATH" >&2
        return 1 2>/dev/null || exit 1
    fi
    # export.sh is chatty and may touch unset variables; keep the caller quiet.
    . "$EINK_IDF_PATH/export.sh" >/dev/null 2>&1
fi

if command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF ready: $(idf.py --version 2>/dev/null | tail -n 1) (IDF_PATH=$IDF_PATH)"
else
    echo "tools/env.sh: idf.py is still not on PATH after sourcing export.sh" >&2
    return 1 2>/dev/null || exit 1
fi

unset EINK_IDF_PATH
