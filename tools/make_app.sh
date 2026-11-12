#!/bin/sh
# Build build/FerrariVR.app from build/fvr.
#   tools/make_app.sh [game-dir] [--embed]
# game-dir (optional): the Windows game folder; its icon is used for the app icon.
# --embed copies the whole game folder into the bundle (Resources/game) for a self-contained app
# on your own machine. Without it the app finds the game next to itself or asks for the folder on
# first launch (remembered in ~/Library/Application Support/FerrariVirtualRace/game_path.txt).
set -e
cd "$(dirname "$0")/.."
GAME=""; EMBED=0
for a in "$@"; do case "$a" in --embed) EMBED=1 ;; *) GAME="$a" ;; esac; done
[ -n "$GAME" ] && [ -d "$GAME" ] && GAME=$(cd "$GAME" && pwd)
APP=build/FerrariVR.app
VERSION=$(git describe --tags --always 2>/dev/null || echo 0.1)
MINOS=$(sed -n 's/.*-mmacosx-version-min=\([0-9.]*\).*/\1/p' Makefile | head -1)

[ -x build/fvr ] || { echo "build/fvr missing: run make first" >&2; exit 1; }
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp build/fvr "$APP/Contents/MacOS/fvr"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@MINOS@/${MINOS:-11.0}/g" packaging/Info.plist > "$APP/Contents/Info.plist"

# bundle SDL3 so the app runs without Homebrew
SDL=$(otool -L build/fvr | awk '/libSDL3/ {print $1}')
if [ -n "$SDL" ] && [ -f "$SDL" ]; then
    cp "$SDL" "$APP/Contents/Frameworks/"
    LIB=$(basename "$SDL")
    chmod u+w "$APP/Contents/Frameworks/$LIB"
    install_name_tool -id "@rpath/$LIB" "$APP/Contents/Frameworks/$LIB"
    install_name_tool -change "$SDL" "@executable_path/../Frameworks/$LIB" "$APP/Contents/MacOS/fvr"
fi

# icon from the game's own executable (not stored in this repository)
if [ -n "$GAME" ] && [ -f "$GAME/FerrariVR.exe" ] && command -v 7zz >/dev/null 2>&1; then
    T=$(mktemp -d)
    (cd "$T" && 7zz e -y "$GAME/FerrariVR.exe" '.rsrc/*/ICON/1.ico' >/dev/null 2>&1) || true
    if [ -f "$T/1.ico" ]; then
        sips -s format png "$T/1.ico" --out "$T/icon.png" >/dev/null 2>&1 || true
        if [ -f "$T/icon.png" ]; then
            mkdir -p "$T/AppIcon.iconset"
            for s in 16 32 128 256 512; do
                sips -z $s $s "$T/icon.png" --out "$T/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
                sips -z $((s*2)) $((s*2)) "$T/icon.png" --out "$T/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
            done
            iconutil -c icns "$T/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns" || true
        fi
    fi
    rm -rf "$T"
fi

if [ "$EMBED" = 1 ]; then
    [ -f "$GAME/FerrariVR_Hi.exe" ] || { echo "--embed needs the game folder (with FerrariVR_Hi.exe)" >&2; exit 1; }
    echo "copying game files into the bundle..."
    ditto "$GAME" "$APP/Contents/Resources/game"
fi

codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc codesign failed" >&2
echo "built $APP"
