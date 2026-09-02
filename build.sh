#!/usr/bin/env bash
# Build everything: the Aroma plugin, and the PC-side test console.
#
# Run from Git Bash, not msys2 - see tools/build-wups.sh for why.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
mkdir -p build tmp

if [ ! -f "${DEVKITPRO:-/c/devkitPro}/wups/lib/libwups.a" ] &&
   [ ! -f /c/devkitPro/wups/lib/libwups.a ]; then
    echo "==> WUPS SDK not installed yet"
    if [ ! -d thirdparty/WiiUPluginSystem ]; then
        mkdir -p thirdparty
        git clone --depth 1 \
            https://github.com/wiiu-env/WiiUPluginSystem.git \
            thirdparty/WiiUPluginSystem
    fi
    bash tools/build-wups.sh
fi

echo "==> plugin"
bash tools/build-plugin.sh

echo
echo "==> fake console (PC-side test rig)"
MINGW="/c/Users/eduar/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
[ -d "$MINGW" ] && export PATH="$PATH:$MINGW"
gcc -O2 -Wall -Wextra -o build/fake_console.exe \
    tools/fake_console.c common/wstr_jpeg.c common/wstr_net.c \
    -lws2_32 -lwinmm
echo "built ... build/fake_console.exe"

echo
echo "done."
echo "  console:  copy build/wiiustream.wps to"
echo "            sd:/wiiu/environments/aroma/plugins/"
echo "  PC:       python pc/dashboard.py"
