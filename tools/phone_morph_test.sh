#!/bin/sh
# 3.1-dev.1: tests setApp's DOM patcher against every phone harness screen.
# 1) renders each harness mode and captures what it drew into #app;
# 2) in one page, patches from screen to screen in many random orders and
#    checks after every step that the patched screen equals the same screen
#    drawn fresh (innerHTML compared).
# Needs build/phone_check/harness.html: run `sh tools/phone_check.sh render` first.
set -e
ROOT="$(cd "$(dirname "$0")/.." && (pwd -W 2>/dev/null || pwd))"
OUT="$ROOT/build/phone_check"
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
H="$ROOT/tools/phone_harness"
sed 's#</body>#<script>setTimeout(()=>{const p=document.createElement("pre");p.id="DUMP";p.textContent=document.getElementById("app").innerHTML;document.body.appendChild(p)},2800)</script></body>#' \
  "$OUT/harness.html" > "$OUT/harness_dump.html"
mkdir -p "$OUT/dumps"
for MODE in $(grep -o '^  [a-z]*: {' "$H/harness_pre.js" | sed 's/[: {]//g'); do
  "$EDGE" --headless=new --disable-gpu --virtual-time-budget=4000 --dump-dom "file:///$OUT/harness_dump.html#$MODE" 2>/dev/null > "$OUT/dumps/$MODE.html" || true
done
# the patcher, cut out of the real page
awk '/^const MORPH_TPL/{f=1} f{print} /^function setApp/{g=1} g&&/^}/{exit}' "$OUT/page.html" > "$OUT/morph.js"
python -I "$ROOT/tools/phone_morph_test.py" "$OUT"
"$EDGE" --headless=new --disable-gpu --virtual-time-budget=8000 --dump-dom "file:///$OUT/morph_test.html" 2>/dev/null | grep -o '<pre id="R">[^<]*' | sed 's/<pre id="R">//'
