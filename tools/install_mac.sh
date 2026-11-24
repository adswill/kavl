#!/bin/bash
# From a fresh download to a running KAVL on a Mac, in one command:   tools/install_mac.sh
# Checks Apple's developer tools and Homebrew, installs the libraries KAVL needs (only the missing ones; nothing already there is upgraded),
# builds the app and opens it. Safe to run again: after an update (git pull, or a new download) it rebuilds only what changed.
# KAVL_NO_OPEN=1 builds without opening the app.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\n\033[31mStopped: %s\033[0m\n' "$1"; exit 1; }

[ "$(uname)" = Darwin ] || fail "this script is for macOS. KAVL runs on macOS only for now."
ver="$(sw_vers -productVersion)"
[ "${ver%%.*}" -ge 12 ] || fail "KAVL needs macOS 12 or later; this Mac has macOS $ver."

step "1/4 Apple's developer tools"
if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install >/dev/null 2>&1 || true
  fail "a window has opened asking to install the Command Line Developer Tools. Click Install, wait until it says it is done (5 to 15 minutes), then run this same command again."
fi
echo "installed"

step "2/4 Homebrew"
BREW="$(command -v brew || true)"
for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -z "$BREW" ] && [ -x "$b" ] && BREW="$b"; done
[ -n "$BREW" ] || fail "Homebrew is not installed. Open https://brew.sh, copy the one line under 'Install Homebrew' into Terminal and press Return, follow what it asks, then run this same command again."
eval "$("$BREW" shellenv)"
echo "found at $BREW"

step "3/4 Libraries"
PKGS=(cmake ninja pkgconf glfw dav1d opus hackrf libusb svt-av1 librtlsdr ffmpeg)
missing=()
for p in "${PKGS[@]}"; do brew list --versions "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
  echo "installing: ${missing[*]} (a few minutes)"
  HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install "${missing[@]}" || fail "Homebrew could not install ${missing[*]}. Run 'brew doctor', fix what it says, then run this same command again."
else
  echo "all installed"
fi

step "4/4 Building KAVL (5 to 10 minutes the first time, seconds after that)"
if [ ! -f build/CMakeCache.txt ]; then
  cmake -S . -B build -G Ninja > build-setup.log 2>&1 || { tail -20 build-setup.log || true; fail "setting up the build failed (the lines above say why; all of it is in build-setup.log)."; }
fi
cmake --build build > build/install.log 2>&1 || { grep -E "error|FAILED" -A3 build/install.log | head -30 || true; fail "the build failed (the lines above say why; all of it is in build/install.log)."; }
rm -f build-setup.log
printf '\n\033[32mDone.\033[0m KAVL is at %s/build/KAVL.app\n' "$ROOT"
echo "Open it again later with:  open \"$ROOT/build/KAVL.app\"   (or double-click it in Finder)"
[ -n "${KAVL_NO_OPEN:-}" ] || open "$ROOT/build/KAVL.app"
