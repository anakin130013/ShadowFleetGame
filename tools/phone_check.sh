#!/bin/sh
# Offline checks for the phone page embedded in web_server.cpp (PAGE_HTML),
# which the Arduino compiler never looks at. Uses headless Microsoft Edge,
# which ships with Windows -- no Node install needed.
#
#   sh tools/phone_check.sh            JS syntax check only
#   sh tools/phone_check.sh render     + render the page with a fake /state
#                                      and a battle-scene contact sheet to
#                                      build/phone_sheet.png
#
# The fake state and contact-sheet scenarios live in tools/phone_harness/.
# NOTE: headless Edge enforces a ~510px minimum viewport even with
# --window-size=400. The harness pins <body> to 380px, so normal screens
# look right, but FULL-SCREEN overlays (position:fixed -- e.g. the round
# banner) center in the 510px viewport and look shifted right in the 400px
# screenshot. On a real phone they center correctly.
set -e
ROOT="$(cd "$(dirname "$0")/.." && (pwd -W 2>/dev/null || pwd))"  # -W: C:/ style path, needed for file:/// URLs
OUT="$ROOT/build/phone_check"
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
mkdir -p "$OUT"
SRC="$ROOT/ShadowFleet_Prototype_02/web_server.cpp"

awk '/R"HTML\(/{f=1; sub(/.*R"HTML\(/,""); } f && /^\)HTML";/{f=0} f' "$SRC" > "$OUT/page.html"
awk '/<script>/{f=1; next} /<\/script>/{f=0} f' "$OUT/page.html" > "$OUT/page.js"

# Syntax: compile (don't run) the script with new Function().
{ echo '<html><body><pre id="out">pending</pre><script type="text/plain" id="src">'
  cat "$OUT/page.js"
  echo '</script><script>try{new Function(document.getElementById("src").textContent);document.getElementById("out").textContent="SYNTAX OK";}catch(e){document.getElementById("out").textContent="SYNTAX ERROR: "+e.message;}</script></body></html>'
} > "$OUT/syntax.html"
RESULT=$("$EDGE" --headless=new --disable-gpu --dump-dom "file:///$OUT/syntax.html" 2>/dev/null | grep -o '<pre id="out">[^<]*' | sed 's/<pre id="out">//')
echo "$RESULT"
case "$RESULT" in "SYNTAX OK") ;; *) exit 1 ;; esac

if [ "$1" = "render" ]; then
  H="$ROOT/tools/phone_harness"
  cp "$ROOT/tools/fonts/SpaceGrotesk-latin.woff2" "$OUT/font.woff2"  # 3.1-dev.4: the table serves it at /font.woff2
  # v0.21.20: real commander art for the fake /art, straight from the firmware data.
  perl "$H/art_from_c.pl" "$ROOT/ShadowFleet_Prototype_02/commander_art.cpp" "$ROOT/ShadowFleet_Prototype_02/commander_art.h" > "$OUT/art.js"
  cat "$OUT/art.js" "$H/harness_pre.js" > "$OUT/pre_all.js"
  awk -v pre="$OUT/pre_all.js" -v post="$H/harness_post.js" '
    /<script>/ && !done { print "<script>"; while ((getline l < pre) > 0) print l; print "</script>"; }
    { print }
    /<\/script>/ && !done { print "<script>"; while ((getline l < post) > 0) print l; print "</script>"; done=1 }' "$OUT/page.html" \
    | sed 's#<body>#<body style="width:380px;margin:0">#' > "$OUT/harness.html"
  "$EDGE" --headless=new --disable-gpu --hide-scrollbars --window-size=2080,1420 --virtual-time-budget=4000 \
    --screenshot="$ROOT/build/phone_sheet.png" "file:///$OUT/harness.html" 2>/dev/null
  echo "wrote build/phone_sheet.png"
  # One phone-sized screenshot per HARNESS_MODES entry in harness_pre.js.
  for MODE in $(grep -o '^  [a-z]*: {' "$H/harness_pre.js" | sed 's/[: {]//g'); do
    "$EDGE" --headless=new --disable-gpu --hide-scrollbars --window-size=400,900 --virtual-time-budget=3000 \
      --screenshot="$ROOT/build/phone_$MODE.png" "file:///$OUT/harness.html#$MODE" 2>/dev/null
    echo "wrote build/phone_$MODE.png"
  done
fi
