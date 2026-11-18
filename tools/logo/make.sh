#!/usr/bin/env bash
# Renders the Farcade logo (docs/logo/farcade-logo.svg) to PNGs and the README
# banners (the mark with the "Farcade Engine" wordmark in Inter, dark and light).
# Needs Node with Playwright and Chromium (as on the dev containers).
set -euo pipefail
cd "$(dirname "$0")/../.."
D=docs/logo
FONT=$(pwd)/editor/fonts
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
node tools/logo/render.cjs $D/farcade-logo.svg $D/farcade-logo-1024.png 1024 1024 1
node tools/logo/render.cjs $D/farcade-logo.svg $D/farcade-logo-256.png 256 256 1
for theme in dark light; do
    if [ $theme = dark ]; then FG="#E8EBF0"; SUB="#9AA3AF"; else FG="#16191E"; SUB="#5D646E"; fi
    cat > "$TMP/banner-$theme.html" <<HTML
<!doctype html><html><head><meta charset="utf-8"><style>
@font-face { font-family: InterM; src: url("file://$FONT/Inter-Medium.otf"); }
@font-face { font-family: InterR; src: url("file://$FONT/Inter-Regular.otf"); }
html,body { margin:0; background: transparent; }
.b { display:flex; align-items:center; gap:28px; padding: 8px 12px; width: 840px; height: 176px; box-sizing: border-box; }
.b svg { width: 152px; height: 152px; flex: none; filter: drop-shadow(0 6px 14px rgba(30,70,160,0.30)); }
.t { display:flex; flex-direction:column; gap: 6px; }
.n { font-family: InterM; font-size: 68px; letter-spacing: -1.5px; color: $FG; line-height: 1; }
.n b { font-weight: normal; color: #4C8DFF; }
.s { font-family: InterR; font-size: 22px; color: $SUB; letter-spacing: 0.2px; }
</style></head><body><div class="b">$(cat $D/farcade-logo.svg)<div class="t"><div class="n">Farcade <b>Engine</b></div><div class="s">A game engine for Minimal-OS</div></div></div></body></html>
HTML
    node tools/logo/render.cjs "$TMP/banner-$theme.html" $D/farcade-banner-$theme.png 840 176 2
done
echo "logo: wrote $D/farcade-logo-{1024,256}.png and $D/farcade-banner-{dark,light}.png"
