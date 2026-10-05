#!/usr/bin/env bash
# Render main/ui.c on the host into build/preview/*.png (needs cc + Pillow).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/preview"
mkdir -p "$OUT"
TZ="CET-1CEST,M3.5.0,M10.5.0/3" cc -std=gnu11 -Wall -Wextra -O1 \
    -I"$ROOT/tools/preview/stub" -I"$ROOT/main" \
    "$ROOT/tools/preview/ui_preview.c" "$ROOT/main/ui.c" "$ROOT/main/ui_assets.c" \
    -lm -o "$OUT/ui_preview"
TZ="CET-1CEST,M3.5.0,M10.5.0/3" "$OUT/ui_preview" "$OUT"
python3 - "$OUT" <<'PY'
import glob, sys
from PIL import Image
for p in sorted(glob.glob(sys.argv[1] + "/*.pgm")):
    Image.open(p).save(p[:-4] + ".png")
    print(p[:-4] + ".png")
PY
