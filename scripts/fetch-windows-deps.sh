#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Fetches everything needed to cross-compile the Windows x64 build from macOS
# or Linux into third_party/windows/:
#   - llvm-mingw (the exact release Qt's win64_llvm_mingw binaries use)
#   - Qt for Windows (qtbase, qtsvg, qtimageformats) + its llvm-mingw runtime
#   - PDFium win-x64 binaries, miniz sources
# Host Qt tools (moc, rcc) of the same Qt version must be installed on the host
# (Homebrew: brew install qtbase); the Windows Qt defaults to that version.
set -euo pipefail
cd "$(dirname "$0")/.."

QT_VER="${QT_VER:-$(qmake -query QT_VERSION 2>/dev/null || qmake6 -query QT_VERSION)}"
MINIZ_VER="${MINIZ_VER:-3.1.0}"
PDFIUM_BUILD="${PDFIUM_BUILD:-8125}"   # pdfium-binaries tag chromium/<build>; pinned so builds are repeatable
DEST=third_party/windows
mkdir -p "$DEST" && cd "$DEST"

QV="${QT_VER//./}"
REPO="https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_${QV}/qt6_${QV}_llvm_mingw"
curl -fsSL "$REPO/Updates.xml" -o Updates.xml
python3 - "$QV" > qt-archives.txt <<'EOF'
import sys, xml.etree.ElementTree as ET
qv = sys.argv[1]
want = {f"qt.qt6.{qv}.win64_llvm_mingw": ("qtbase", "qtsvg", "llvm-mingw"),
        f"qt.qt6.{qv}.addons.qtimageformats.win64_llvm_mingw": ("qtimageformats",)}
for p in ET.parse("Updates.xml").getroot().findall("PackageUpdate"):
    name = p.findtext("Name")
    if name not in want: continue
    for a in (p.findtext("DownloadableArchives") or "").split(","):
        a = a.strip()
        if a.startswith(want[name]):
            print(name, p.findtext("Version"), a)
EOF
# The compiler must be the llvm-mingw release this Qt was built with; its
# runtime archive is named after it (llvm-mingw-<release>-ucrt-...).
LLVM_MINGW="${LLVM_MINGW:-$(sed -n 's/.*llvm-mingw-\([0-9]*\)-ucrt.*/\1/p' qt-archives.txt | head -1)}"
[[ -n "$LLVM_MINGW" ]] || { echo "no llvm-mingw runtime listed for Qt $QT_VER" >&2; exit 1; }
echo "Qt $QT_VER for Windows, llvm-mingw $LLVM_MINGW"
# Switching versions: start over with matching downloads.
[[ "$(cat qt/VERSION 2>/dev/null)" == "$QT_VER" ]] || rm -rf qt
[[ "$(cat llvm-mingw/RELEASE 2>/dev/null)" == "$LLVM_MINGW" ]] || rm -rf llvm-mingw

case "$(uname -s)-$(uname -m)" in
  Darwin-*) host=macos-universal ;;
  Linux-x86_64) host=ubuntu-22.04-x86_64 ;;
  Linux-aarch64) host=ubuntu-22.04-aarch64 ;;
  *) echo "unsupported host" >&2; exit 1 ;;
esac

if [[ ! -x llvm-mingw/bin/x86_64-w64-mingw32-clang++ ]]; then
  echo "Fetching llvm-mingw $LLVM_MINGW ($host)..."
  curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/llvm-mingw-$LLVM_MINGW-ucrt-$host.tar.xz" \
    | tar xJ
  rm -rf llvm-mingw && mv "llvm-mingw-$LLVM_MINGW-ucrt-$host" llvm-mingw
  echo "$LLVM_MINGW" > llvm-mingw/RELEASE
  # Downloaded binaries are quarantined on macOS; the toolchain is ours to run.
  xattr -dr com.apple.quarantine llvm-mingw 2>/dev/null || true
fi

if [[ ! -f qt/bin/Qt6Core.dll ]]; then
  echo "Fetching Qt $QT_VER for Windows (llvm-mingw)..."
  mkdir -p qt-dl qt-stage qt-runtime
  while read -r name ver archive; do
    url="$REPO/$name/$ver$archive"
    curl -fsSL "$url" -o "qt-dl/$archive"
    expected="$(curl -fsSL "$url.sha1" | awk '{print $1}')"
    actual="$(shasum -a 1 "qt-dl/$archive" | awk '{print $1}')"
    [[ "$expected" == "$actual" ]] || { echo "checksum mismatch: $archive" >&2; exit 1; }
    case "$archive" in
      llvm-mingw*.zip) unzip -qo "qt-dl/$archive" -d qt-runtime ;;
      llvm-mingw*) 7zz x -y -bso0 "qt-dl/$archive" -oqt-runtime >/dev/null ;;
      *) 7zz x -y -bso0 "qt-dl/$archive" -oqt-stage >/dev/null ;;
    esac
  done < qt-archives.txt
  # Archives unpack either at the top level or under <ver>/llvm-mingw_64.
  root="$(cd "$(dirname "$(find qt-stage -path '*/bin/Qt6Core.dll' | head -1)")/.." && pwd)"
  rm -rf qt && mv "$root" qt
  mkdir -p qt/runtime && find qt-runtime -name '*.dll' -exec cp {} qt/runtime/ \;
  echo "$QT_VER" > qt/VERSION
  rm -rf qt-stage qt-runtime qt-dl
fi
rm -f Updates.xml qt-archives.txt

if [[ ! -f pdfium/bin/pdfium.dll ]]; then
  echo "Fetching PDFium (win-x64)..."
  mkdir -p pdfium
  curl -fsSL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F$PDFIUM_BUILD/pdfium-win-x64.tgz" \
    | tar xz -C pdfium
fi

if [[ ! -f miniz/miniz.c ]]; then
  echo "Fetching miniz $MINIZ_VER..."
  curl -fsSL "https://github.com/richgel999/miniz/releases/download/$MINIZ_VER/miniz-$MINIZ_VER.zip" -o miniz.zip
  rm -rf miniz && unzip -qo miniz.zip -d miniz && rm miniz.zip
fi
echo "Windows dependencies ready in $DEST"
