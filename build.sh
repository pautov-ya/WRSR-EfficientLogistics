#!/bin/sh
# Build constructlog.dll and deploy it into its Workshop project folder.
#   ./build.sh          build + deploy
#   ./build.sh nodeploy build only
set -e
cd "$(dirname "$0")"

CXX=C:/WRSR-Dev/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
GAME=D:/SteamLibrary/steamapps/common/SovietRepublic
# The RML development project; becomes the Workshop id once the item exists.
PROJECT=9784154

mkdir -p build
"$CXX" -O2 -Wall -Wno-unused-function -static -shared -o build/constructlog.dll src/constructlog.cpp -luser32 -lshlwapi
cp -f src/constructlog.ini build/constructlog.ini
echo "[build] build/constructlog.dll"

[ "$1" = "nodeploy" ] && exit 0
[ -n "$PROJECT" ] || { echo "[deploy] no project folder yet - create the item in the game's Workshop menu and set PROJECT"; exit 0; }

DIR="$GAME/media_soviet/workshop_wip/$PROJECT"
mkdir -p "$DIR/plugins"
cp -f workshopconfig.ini "$DIR/workshopconfig.ini"
cp -f build/constructlog.dll build/constructlog.ini "$DIR/plugins/"
echo "[deploy] $DIR"
