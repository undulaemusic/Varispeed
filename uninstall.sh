#!/bin/bash
# Removes Varispeed completely: the driver, the app and its settings, then restarts Core Audio.
# Your recordings are never deleted.
set -euo pipefail
DRV_DEST="/Library/Audio/Plug-Ins/HAL/Varispeed.driver"

osascript -e 'quit app id "com.undulaemusic.Varispeed"' >/dev/null 2>&1 || true
rm -rf "/Applications/Varispeed.app" 2>/dev/null || sudo rm -rf "/Applications/Varispeed.app"
defaults delete com.undulaemusic.Varispeed >/dev/null 2>&1 || true
rm -f "$HOME/Library/Preferences/com.undulaemusic.Varispeed.plist"

if [ -d "$DRV_DEST" ]; then
  echo "Removing the Varispeed driver (macOS will ask for your password)."
  echo "Core Audio restarts afterwards: all audio stops for a few seconds."
  sudo rm -rf "$DRV_DEST"
  sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
fi
echo "Varispeed is fully removed. (Your recordings were left where they are.)"
