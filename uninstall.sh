#!/bin/bash
# Removes Varispeed.driver and restarts Core Audio. Touches nothing else.
set -euo pipefail
DEST="/Library/Audio/Plug-Ins/HAL/Varispeed.driver"

if [ -d "$DEST" ]; then
  echo "Removing Varispeed driver (needs your Mac password)."
  sudo rm -rf "$DEST"
  echo "Restarting Core Audio. All audio will cut out for a few seconds."
  sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
fi

# Remove the menu bar app and its settings if they were installed.
rm -rf "/Applications/Varispeed.app" 2>/dev/null || true
defaults delete com.undulaemusic.Varispeed 2>/dev/null || true
rm -f "$HOME/Library/Preferences/com.undulaemusic.Varispeed.plist" \
      "$HOME/Library/Preferences/com.undulaemusic.Varispeed.plist" 2>/dev/null || true   # pre-rename settings
echo "Varispeed is fully removed."
