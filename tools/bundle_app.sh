#!/bin/bash
# Makes KAVL.app self-contained: puts ffmpeg and ffprobe (tools/build_ffmpeg.sh) in Contents/MacOS, copies every non-system dylib the app and
# those two use into Contents/Frameworks, points them there (@executable_path/../Frameworks), adds the licence texts and signs it ad hoc.
# Usage: tools/bundle_app.sh build/KAVL.app
set -euo pipefail
APP="${1:?usage: tools/bundle_app.sh path/to/KAVL.app}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FF="$ROOT/build-ffmpeg/out/bin"
MAC="$APP/Contents/MacOS"
FW="$APP/Contents/Frameworks"
# nothing changed since the last bundle (the app binary as signed then, ffmpeg, the licence texts): done (saves a second on every build)
STAMP="$APP/Contents/.bundle-stamp"
key() { stat -f "%m:%z" "$MAC/KAVL" "$FF/ffmpeg" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" 2>/dev/null | tr '\n' ' '; }
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$(key)" ]; then
  echo "bundle up to date"
  exit 0
fi
mkdir -p "$FW" "$APP/Contents/Resources/licences"
if [ -x "$FF/ffmpeg" ]; then
  cp -f "$FF/ffmpeg" "$FF/ffprobe" "$MAC/"
  chmod u+w "$MAC/ffmpeg" "$MAC/ffprobe"
else
  echo "note: no bundled ffmpeg (run tools/build_ffmpeg.sh first); the app will look for a Homebrew one"
fi
is_external() { case "$1" in /usr/lib/*|/System/*|@*) return 1;; *) return 0;; esac; }
# the binaries to fix, and the queue of dylibs still to copy
todo=()
for b in "$MAC"/*; do [ -f "$b" ] && file "$b" | grep -q Mach-O && todo+=("$b"); done
done_libs=" "
while [ ${#todo[@]} -gt 0 ]; do
  bin="${todo[0]}"; todo=("${todo[@]:1}")
  while read -r dep; do
    is_external "$dep" || continue
    real="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$dep")"
    name="$(basename "$dep")"
    if [[ "$done_libs" != *" $name "* ]]; then
      cp -f "$real" "$FW/$name"
      chmod u+w "$FW/$name"
      install_name_tool -id "@executable_path/../Frameworks/$name" "$FW/$name" 2>/dev/null
      done_libs+="$name "
      todo+=("$FW/$name")
    fi
    install_name_tool -change "$dep" "@executable_path/../Frameworks/$name" "$bin" 2>/dev/null
  done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -v "^$(basename "$bin")$" || true)
done
# licence texts of what is bundled
L="$APP/Contents/Resources/licences"
cp -f "$ROOT/LICENSE" "$L/KAVL-LICENSE.txt"
cp -f "$ROOT/THIRD_PARTY.md" "$L/THIRD_PARTY.md"
[ -f "$ROOT/build-ffmpeg/src/COPYING.LGPLv2.1" ] && cp -f "$ROOT/build-ffmpeg/src/COPYING.LGPLv2.1" "$L/FFmpeg-COPYING.LGPLv2.1.txt"
codesign --force --sign - "$FW"/*.dylib "$MAC"/ffmpeg "$MAC"/ffprobe 2>/dev/null || true
codesign --force --deep --sign - "$APP"
key > "$STAMP"
echo "bundled:$done_libs"
