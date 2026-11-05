#!/bin/sh
# Renders the real UI code on the PC (software rasteriser standing in for vita2d,
# FreeType standing in for the Vita system font) against a fake Jellyfin server,
# and writes screenshots to tools/preview/shots/. Usage: tools/preview/run.sh [theme 0-4]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src"; VITASDK=${VITASDK:-$HOME/vitasdk}
INC="$HERE/.inc"; mkdir -p "$INC" "$HERE/shots"
for d in psp2 psp2common vitasdk vitasdk.h vita2d.h; do ln -sfn "$VITASDK/arm-vita-eabi/include/$d" "$INC/$d"; done
cc -std=gnu11 -O2 -w -D__vita__ -I"$INC" -I"$HERE" -I/usr/include/freetype2 -I"$SRC" \
  "$HERE"/drive.c "$HERE"/vita2d_host.c "$HERE"/font_host.c "$HERE"/sce_host.c \
  "$SRC"/ui.c "$SRC"/screens.c "$SRC"/images.c "$SRC"/input.c "$SRC"/jellyfin.c "$SRC"/http.c "$SRC"/json.c "$SRC"/config.c "$SRC"/sfx.c "$SRC"/subs.c \
  -lcurl -lpthread -ljpeg -lfreetype -lm -o "$HERE/.drive"
python3 "$HERE/fakejf.py" 18790 & SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
cd "$HERE" && ./.drive "${1:-0}" "${2:-}" | grep '^shot'
for f in shots/*.ppm; do python3 -c "from PIL import Image; Image.open('$f').save('${f%.ppm}.png')" && rm "$f"; done
