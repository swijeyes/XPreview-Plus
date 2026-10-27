#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-compiles the Windows x64 build and packages it as a portable zip
# (no installer, no registry: settings live next to the executable).
# Usage: scripts/package-windows.sh [OUTPUT_DIR]   (default: release/)
# Requires host Qt tools of the same version as the Windows Qt (brew install qtbase).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-release}"
W="$PWD/third_party/windows"
QT_HOST="${QT_HOST_PATH:-$(brew --prefix 2>/dev/null || echo /usr)}"

./scripts/fetch-deps.sh >/dev/null   # fonts
./scripts/fetch-windows-deps.sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw-x64.cmake \
      -DQT_HOST_PATH="$QT_HOST" -DCMAKE_PREFIX_PATH="$W/qt" -DPDFIUM_DIR="$W/pdfium"
cmake --build build-win --target XPreview

NAME="$(sed -n 's/^set(APP_DISPLAY_NAME "\(.*\)")/\1/p' CMakeLists.txt)"
VERSION="$(sed -n 's/^project(XPreview VERSION \([0-9]*\.[0-9]*\).*/\1/p' CMakeLists.txt)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$NAME"
mkdir -p "$APP"/{platforms,styles,iconengines,imageformats,licenses}

cp "build-win/$NAME.exe" "$APP/"
cp "$W"/qt/bin/Qt6{Core,Gui,Widgets,Svg,PrintSupport}.dll "$APP/"
cp "$W"/qt/runtime/{libc++,libunwind}.dll "$APP/"
cp "$W/pdfium/bin/pdfium.dll" "$APP/"
cp "$W/qt/plugins/platforms/qwindows.dll" "$APP/platforms/"
cp "$W/qt/plugins/styles/qmodernwindowsstyle.dll" "$APP/styles/"
cp "$W/qt/plugins/iconengines/qsvgicon.dll" "$APP/iconengines/"
for f in qgif qico qjpeg qsvg qtiff qwebp qwbmp qtga qicns; do
  cp "$W/qt/plugins/imageformats/$f.dll" "$APP/imageformats/"
done
mkdir -p "$APP/fonts" && cp resources/fonts/*.ttf "$APP/fonts/"
cp LICENSE THIRD_PARTY_NOTICES.md "$APP/"
[[ -f manual/xpreview-plus-manual.pdf ]] && cp manual/xpreview-plus-manual.pdf "$APP/$NAME User Manual.pdf"
cp -R licenses/. "$APP/licenses/"
touch "$APP/portable"   # settings go to .\settings\ instead of the registry

# Every DLL any shipped binary imports must be shipped or be a Windows
# system DLL (catches a missing dependency without a Windows machine).
RO="$W/llvm-mingw/bin/llvm-readobj"
shipped="$(find "$APP" -iname '*.dll' -exec basename {} \; | tr 'A-Z' 'a-z' | sort -u)"
missing="$(find "$APP" \( -iname '*.exe' -o -iname '*.dll' \) -exec "$RO" --coff-imports {} \; \
  | sed -n 's/^ *Name: \(.*\.dll\)$/\1/Ip' | tr 'A-Z' 'a-z' | sort -u \
  | grep -vxF -f <(echo "$shipped") \
  | grep -vE '^api-ms-win-' \
  | grep -vE '^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|comdlg32|winspool|uxtheme|dwmapi|imm32|winmm|ws2_32|version|shlwapi|shcore|setupapi|netapi32|userenv|wtsapi32|mpr|authz|ntdll|dwrite|d3d9|d3d11|d3d12|dxgi|mscms|crypt32|bcrypt|secur32|iphlpapi|dnsapi|comctl32|msvcrt|ucrtbase|rpcrt4|propsys|cfgmgr32|powrprof|dbghelp)\.dll$' || true)"
if [[ -n "$missing" ]]; then echo "error: unresolved DLL imports: $missing" >&2; exit 1; fi

mkdir -p "$OUT"
ZIP="$OUT/XPreviewPlus-$VERSION-windows-x64-portable.zip"
rm -f "$ZIP"
(cd "$STAGE" && zip -qr -9 -X "$OLDPWD/$ZIP" "$NAME")
(cd "$(dirname "$ZIP")" && shasum -a 256 "$(basename "$ZIP")" | tee "$(basename "$ZIP").sha256")
echo "Built $ZIP"
