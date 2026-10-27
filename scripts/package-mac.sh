#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds a release XPreview+.app with Qt bundled, signs it with the hardened
# runtime, and produces a drag-to-install DMG.
# Usage: scripts/package-mac.sh [OUTPUT_DIR]   (default: release/)
#   SIGN_ID="Developer ID Application: ..." to sign for distribution
#   (default is ad-hoc, which runs on this Mac; notarise with notarytool).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-release}"
SIGN_ID="${SIGN_ID:--}"
NAME="$(sed -n 's/^set(APP_DISPLAY_NAME "\(.*\)")/\1/p' CMakeLists.txt)"
VERSION="$(sed -n 's/^project(XPreview VERSION \([0-9]*\.[0-9]*\).*/\1/p' CMakeLists.txt)"

./scripts/fetch-deps.sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build-release --target XPreview xpreview-tests xpreview-ui-tests
# Refuse to package a failing build (unit, security and UI layout tests).
run_tests() {  # quiet on success, full log on failure
  local log; log="$(mktemp)"
  if ! "$@" >"$log" 2>&1; then
    grep -E 'FAIL|Loc:|Totals|signal' "$log" >&2 || tail -20 "$log" >&2
    echo "error: tests failed: $*" >&2
    exit 1
  fi
  rm -f "$log"
}
run_tests env QT_QPA_PLATFORM=offscreen build-release/xpreview-tests
run_tests build-release/xpreview-ui-tests -platform offscreen

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$NAME.app"
cp -R "build-release/$NAME.app" "$APP"

# Bundle Qt frameworks + plugins (image formats incl. WebP/TIFF/HEIC, SVG,
# print support) and rewrite install names.
"$(brew --prefix)/bin/macdeployqt" "$APP" -always-overwrite -libpath="$PWD/third_party/pdfium/lib"

# macdeployqt leaves some copied dylibs with their Homebrew install-name id.
for lib in "$APP"/Contents/Frameworks/*.dylib; do
  id="$(otool -D "$lib" | tail -1)"
  if [[ "$id" == /opt/homebrew/* || "$id" == /usr/local/* ]]; then
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null
  fi
done
# License texts of everything bundled (LGPL/BSD/MIT require them with
# binaries): ours, then each Homebrew package macdeployqt copied in.
LIC="$APP/Contents/Resources/licenses"
mkdir -p "$LIC" && cp -R licenses/. "$LIC/"
for lib in "$APP"/Contents/Frameworks/*.dylib; do
  base="$(basename "$lib")"
  keg=""
  for d in "$(brew --prefix)"/opt/*/lib/"$base"; do
    if [[ -e "$d" ]]; then keg="$(basename "$(dirname "$(dirname "$d")")")"; break; fi
  done
  [[ -z "$keg" ]] && continue
  mkdir -p "$LIC/$keg"
  find "$(brew --prefix)/opt/$keg/" -maxdepth 1 -type f \( -iname 'licen*' -o -iname 'copying*' -o -iname 'notice*' \) \
    -exec cp {} "$LIC/$keg/" \;
done
# glib's keg ships no license file; it is LGPL-2.1-or-later.
[[ -d "$LIC/glib" ]] && cp licenses/LGPL-2.1.txt "$LIC/glib/"

# Nothing may still point outside the bundle.
if find "$APP" -type f \( -name '*.dylib' -o -perm +111 \) -exec otool -L {} \; 2>/dev/null \
    | grep -E '^\s+(/opt/homebrew|/usr/local)/'; then
  echo "error: bundle still references Homebrew paths" >&2
  exit 1
fi

# Sign inside-out with the hardened runtime (no JIT, no unsigned executable
# memory, no DYLD_* injection). With a Developer ID, library validation is
# also enforced and no entitlements are requested. Ad-hoc signatures carry
# no Team ID, so library validation would reject the bundled frameworks;
# only in that case is it relaxed.
ENT_ARGS=()
if [[ "$SIGN_ID" == "-" ]]; then
  cat > "$STAGE/adhoc.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
</dict></plist>
PLIST
  ENT_ARGS=(--entitlements "$STAGE/adhoc.entitlements")
fi
find "$APP/Contents" \( -name '*.dylib' -o -name '*.framework' \) -print0 | sort -rz |
  xargs -0 -n1 codesign --force --options runtime --timestamp=none --sign "$SIGN_ID"
codesign --force --options runtime --timestamp=none "${ENT_ARGS[@]}" --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP"
# The signed bundle must actually launch (catches signing/loader problems).
if ! "$APP/Contents/MacOS/$NAME" --screenshot "$STAGE/smoke.png" "$PWD/tests/corpus/rich.fodt" \
    2>/dev/null | grep -q "Page 1 of"; then
  echo "error: signed app failed its launch smoke test" >&2
  exit 1
fi

# Drag-to-Applications disk image.
mkdir -p "$OUT"
# "+" is avoided in file names: GitHub rewrites it in release asset names.
DMG="$OUT/XPreviewPlus-$VERSION-macos-$(uname -m).dmg"
mkdir "$STAGE/dmg"
mv "$APP" "$STAGE/dmg/"
ln -s /Applications "$STAGE/dmg/Applications"
[[ -f manual/xpreview-plus-manual.pdf ]] && cp manual/xpreview-plus-manual.pdf "$STAGE/dmg/$NAME User Manual.pdf"
rm -f "$DMG"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$STAGE/dmg" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
if [[ "$SIGN_ID" != "-" ]]; then codesign --force --timestamp --sign "$SIGN_ID" "$DMG"; fi
hdiutil verify "$DMG" >/dev/null
(cd "$(dirname "$DMG")" && shasum -a 256 "$(basename "$DMG")" | tee "$(basename "$DMG").sha256")
echo "Built $DMG"
