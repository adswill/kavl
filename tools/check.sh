#!/bin/bash
# One command for the usual checks, about 15 s:
#   1. build (niced, 3 jobs),
#   2. the fast tests in parallel (everything but the slow threshold sweep),
#   3. a short run of the app itself, hidden: the test pattern sent to its own receiver (loopback). It fails when the receiver does not lock,
#      no picture comes, or the delay is above MAX_DELAY seconds.
# Usage: tools/check.sh            options: SECS=10 PROFILE=0 MAX_DELAY=1.5 tools/check.sh;  SKIP_APP=1 for build + tests only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECS="${SECS:-10}"; PROFILE="${PROFILE:-0}"; MAX_DELAY="${MAX_DELAY:-1.5}"
t0=$(date +%s)
echo "== build"
nice -n 19 cmake --build build -j3 > build/check-build.log 2>&1 || { grep -E "error:" -A3 build/check-build.log | head -30; echo "BUILD FAILED"; exit 1; }
echo "== tests"
(cd build && ctest -E thresholds -j4 --output-on-failure | tail -3) || { echo "TESTS FAILED"; exit 1; }
[ ! -d build/KAVL.app ] || codesign -v build/KAVL.app || { echo "APP SIGNATURE BROKEN (something changed the bundle after signing)"; exit 1; }
[ -n "${SKIP_APP:-}" ] && { echo "OK ($(( $(date +%s) - t0 )) s, app run skipped)"; exit 0; }
echo "== app over loopback ($SECS s, profile $PROFILE)"
# a copy named KAVLcheck (an instant APFS clone), so it never mixes with a KAVL you have open; hidden runs read and write no settings
C="$ROOT/build/check"
rm -rf "$C/KAVLcheck.app" && mkdir -p "$C"
cp -c -R build/KAVL.app "$C/KAVLcheck.app" 2>/dev/null || cp -R build/KAVL.app "$C/KAVLcheck.app"
mv "$C/KAVLcheck.app/Contents/MacOS/KAVL" "$C/KAVLcheck.app/Contents/MacOS/KAVLcheck"
"$C/KAVLcheck.app/Contents/MacOS/KAVLcheck" --hidden --mute --loopback --profile "$PROFILE" --report --run-for "$SECS" > "$C/report.txt" 2>&1 || true
grep -E "^EVENT" "$C/report.txt" | sed 's/^/   /'
locked=$(grep -m1 "EVENT .* locked" "$C/report.txt" | sed -E 's/.*t=([0-9.]+).*/\1/' || true)
picture=$(awk '/EVENT .* locked/{l=1} l && /EVENT .* picture$/{sub(/.*t=/,""); sub(/ .*/,""); print; exit}' "$C/report.txt")
delay=$(grep -E "^t=" "$C/report.txt" | tail -1 | sed -E 's/.*delay=([-0-9.]+).*/\1/')
lost=$(grep -E "^t=" "$C/report.txt" | tail -1 | sed -E 's/.* lost=([0-9]+).*/\1/')
echo "   locked at ${locked:-never} s, picture at ${picture:-never} s, delay ${delay:-?} s, lost ${lost:-?} code words"
fail=""
[ -z "$locked" ] && fail="no lock"
[ -z "$picture" ] && fail="$fail no picture"
awk -v d="${delay:--1}" -v m="$MAX_DELAY" 'BEGIN{exit !(d < 0 || d > m)}' && fail="$fail delay ${delay} s > ${MAX_DELAY} s"
[ -n "$fail" ] && { echo "APP CHECK FAILED:$fail (report: $C/report.txt)"; exit 1; }
echo "OK ($(( $(date +%s) - t0 )) s)"
