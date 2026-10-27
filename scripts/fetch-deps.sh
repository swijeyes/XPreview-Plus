#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Fetches third-party binaries that are not in the system package manager:
#   - PDFium prebuilt binaries (BSD/Apache) from bblanchon/pdfium-binaries
#   - Metric-compatible fonts: Arimo, Tinos, Cousine (Apache-2.0), Carlito, Caladea (SIL OFL)
# Usage: scripts/fetch-deps.sh [mac-arm64|mac-x64|linux-x64|win-x64]
set -euo pipefail
cd "$(dirname "$0")/.."

platform="${1:-}"
if [[ -z "$platform" ]]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) platform=mac-arm64 ;;
    Darwin-x86_64) platform=mac-x64 ;;
    Linux-x86_64) platform=linux-x64 ;;
    Linux-aarch64) platform=linux-arm64 ;;
    *) platform=win-x64 ;;
  esac
fi

if [[ ! -f third_party/pdfium/include/fpdfview.h ]]; then
  echo "Fetching PDFium ($platform)..."
  mkdir -p third_party/pdfium
  curl -fsSL "https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-$platform.tgz" \
    | tar xz -C third_party/pdfium
fi
if [[ "$(uname -s)" == Darwin ]]; then
  # The prebuilt dylib has install name "./libpdfium.dylib"; make it
  # rpath-relative so the app bundle and build tree can both find it.
  install_name_tool -id @rpath/libpdfium.dylib third_party/pdfium/lib/libpdfium.dylib
fi

mkdir -p resources/fonts
fetch_font() {  # url file
  [[ -f "resources/fonts/$2" ]] || curl -fsSL -o "resources/fonts/$2" "$1" || echo "warning: could not fetch $2"
}
gf=https://github.com/google/fonts/raw/main/ofl
fetch_font "$gf/carlito/Carlito-Regular.ttf" Carlito-Regular.ttf
fetch_font "$gf/carlito/Carlito-Bold.ttf" Carlito-Bold.ttf
fetch_font "$gf/carlito/Carlito-Italic.ttf" Carlito-Italic.ttf
fetch_font "$gf/carlito/Carlito-BoldItalic.ttf" Carlito-BoldItalic.ttf
fetch_font "$gf/caladea/Caladea-Regular.ttf" Caladea-Regular.ttf
fetch_font "$gf/caladea/Caladea-Bold.ttf" Caladea-Bold.ttf
fetch_font "$gf/caladea/Caladea-Italic.ttf" Caladea-Italic.ttf
fetch_font "$gf/caladea/Caladea-BoldItalic.ttf" Caladea-BoldItalic.ttf

# Arimo / Tinos / Cousine (ChromeOS core fonts 1.31, Apache-2.0): metric-
# compatible with Arial / Times New Roman / Courier New. Used instead of
# Liberation 2.x, whose faces all share one "unique ID" (name ID 3); Qt's PDF
# writer de-duplicates embedded fonts by it, so exported PDFs lost bold/italic.
CROS_VER=1.31.0
CROS_SHA256=672c3487883ec1ef83d9254240d4327b014212abc823d06d15816095867315e1
if [[ ! -f resources/fonts/Tinos-Regular.ttf ]]; then
  tmp="$(mktemp -d)"
  curl -fsSL "https://commondatastorage.googleapis.com/chromeos-localmirror/distfiles/croscorefonts-$CROS_VER.tar.bz2" \
    -o "$tmp/cros.tar.bz2"
  echo "$CROS_SHA256  $tmp/cros.tar.bz2" | shasum -a 256 -c - >/dev/null || { echo "error: croscore checksum mismatch" >&2; exit 1; }
  tar xjf "$tmp/cros.tar.bz2" -C "$tmp"
  cp "$tmp/croscorefonts-$CROS_VER"/{Arimo,Tinos,Cousine}-*.ttf resources/fonts/
  rm -rf "$tmp"
fi
rm -f resources/fonts/Liberation*.ttf resources/fonts/LICENSE-Liberation.txt  # superseded
fetch_font "$gf/carlito/OFL.txt" LICENSE-Carlito.txt
fetch_font "$gf/caladea/OFL.txt" LICENSE-Caladea.txt
echo "Done."
