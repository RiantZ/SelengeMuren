#!/usr/bin/env bash
# Headless smoke check of the POC in Chrome through playwright-cli (npm install -g @playwright/cli):
# loads the page, drags, zooms out to the whole recording, runs the stress case and zooms in to microseconds,
# prints the HUD lines and saves screenshots.
#
# Start the server first:   python3 tests/ui/scripts/serve.py
# Usage:                    tests/ui/scripts/smoke_check.sh [url] [out_dir]
set -euo pipefail

URL="${1:-http://127.0.0.1:8765/}"
OUT="${2:-$(mktemp -d -t p8_ui_smoke)}"
mkdir -p "$OUT"
cd "$OUT"                                          # playwright-cli writes .playwright-cli/ into the current dir

LINES='холст|кадры|интервал|рывки|JS/|GPU/|отрисовка|окно|ответ|без данных'

pw() { playwright-cli "$@" >/dev/null 2>&1; }
js() { playwright-cli eval "$1" 2>&1 | sed -n 2p | sed 's/^"//; s/"$//; s/\\n/\n/g; s/\\t/ | /g'; }
hud() { echo "== $1"; js "document.getElementById('hud').textContent" | grep -E "$LINES" || true; }
pick() { js "(() => { const s = document.getElementById('$1'); s.value = '$2'; s.dispatchEvent(new Event('change')); return 'ok'; })()" >/dev/null; }
drag() {                                           # left button from x=1200 to x=400 at y=$1
  pw mousemove 1200 "$1"; pw mousedown
  for x in 1150 1100 1000 900 800 700 600 500 400; do pw mousemove "$x" "$1"; done
  pw mouseup
}

pw open "$URL"
pw resize 1600 1000
sleep 3
err=$(js "document.getElementById('err').textContent")
if [ -n "$err" ]; then echo "page error: $err"; fi

hud "initial: 9 counters, window 2.73 s"
pw screenshot --filename=initial.png

drag 400
sleep 1
hud "after a drag"

pw mousemove 800 400
for i in $(seq 12); do pw mousewheel 500 0; done   # playwright-cli: the first argument is the vertical delta
sleep 2
hud "zoomed out to the whole recording"
pw screenshot --filename=zoom_out.png

pick sel-n 72
pick sel-mult 10000
pw press Home
sleep 2
drag 300
sleep 2
hud "stress: 72 counters at x10000"
pw screenshot --filename=stress.png

pick sel-n 9
pw press Home
pw mousemove 800 450
for i in $(seq 20); do pw press Equal; done
sleep 2
hud "zoomed in to microseconds at x10000"
pw screenshot --filename=zoom_in.png

pw close
echo "screenshots: $OUT"
