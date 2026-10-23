#!/bin/sh
# Build efficientlogistics.dll and deploy it into its Workshop item.
#   ./build.sh          build + deploy
#   ./build.sh nodeploy build only
set -e
cd "$(dirname "$0")"

CXX=C:/WRSR-Dev/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
GAME=D:/SteamLibrary/steamapps/common/SovietRepublic
# The Workshop item, created in the game's Workshop menu.
DIR="$GAME/media_soviet/workshop_wip/3825558097"

mkdir -p build
"$CXX" -O2 -Wall -Wno-unused-function -static -shared -o build/efficientlogistics.dll src/efficientlogistics.cpp -luser32 -lshlwapi
cp -f src/efficientlogistics.ini build/efficientlogistics.ini
echo "[build] build/efficientlogistics.dll"

[ "$1" = "nodeploy" ] && exit 0

# workshopconfig.ini here carries the item's $ITEM_ID / $OWNER_ID: without
# them the next upload would publish a new Workshop item.
mkdir -p "$DIR/plugins"
cp -f workshopconfig.ini "$DIR/workshopconfig.ini"
cp -f art/previewimage.png "$DIR/previewimage.png"
cp -f build/efficientlogistics.dll build/efficientlogistics.ini "$DIR/plugins/"
echo "[deploy] $DIR"
