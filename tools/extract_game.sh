#!/bin/sh
# Extract the Windows installer of Ferrari Virtual Race (NSIS) into a folder, no Windows needed.
#   tools/extract_game.sh ferrari-virtual-race-1-0-en-win.exe ~/Games/FerrariVR
# Needs 7-Zip: brew install sevenzip
set -e
[ $# -eq 2 ] || { echo "usage: $0 <installer.exe> <output-folder>" >&2; exit 1; }
Z=$(command -v 7zz || command -v 7z || true)
[ -n "$Z" ] || { echo "7-Zip not found: brew install sevenzip" >&2; exit 1; }
mkdir -p "$2"
"$Z" x -y -o"$2" "$1" >/dev/null
rm -rf "$2/\$PLUGINSDIR"
[ -f "$2/FerrariVR_Hi.exe" ] && echo "game extracted to $2" || { echo "FerrariVR_Hi.exe not found after extraction" >&2; exit 1; }
