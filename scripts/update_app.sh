#!/bin/bash
# Rebuilds the menu bar app and swaps it into /Applications (no password, no Core Audio restart).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/scripts/build_app.sh"
osascript -e 'quit app id "com.undulaemusic.Varispeed"' >/dev/null 2>&1 || true
sleep 1
rm -rf /Applications/Varispeed.app
cp -R "$ROOT/build/Varispeed.app" /Applications/Varispeed.app
open /Applications/Varispeed.app
echo "Varispeed app updated and reopened."
