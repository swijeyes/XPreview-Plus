#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds qpdf (Apache-2.0), which XPreview+ uses to write encrypted PDFs, from
# a pinned and checksummed source release into third_party/qpdf. For systems
# whose package manager has no recent qpdf (the Linux release builds run on
# Ubuntu 22.04, which has 10.6); macOS builds use Homebrew's qpdf instead.
# Needs a C++20 compiler, CMake, and the zlib and libjpeg development files.
# Usage: scripts/build-qpdf.sh       then configure XPreview+ with
#        -DCMAKE_PREFIX_PATH="...;$PWD/third_party/qpdf"
set -euo pipefail
cd "$(dirname "$0")/.."

QPDF_VERSION=12.4.2
QPDF_SHA256=8a58af5b6141319287c1883bec8bd1bd545b7567b7fc5e6ce5d25a1c85f36397
PREFIX="$PWD/third_party/qpdf"

if [[ "$(cat "$PREFIX/VERSION" 2>/dev/null)" == "$QPDF_VERSION" ]]; then
  echo "qpdf $QPDF_VERSION is already built"
  exit 0
fi
echo "Building qpdf $QPDF_VERSION..."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/qpdf.tar.gz" \
  "https://github.com/qpdf/qpdf/releases/download/v$QPDF_VERSION/qpdf-$QPDF_VERSION.tar.gz"
echo "$QPDF_SHA256  $tmp/qpdf.tar.gz" | shasum -a 256 -c - >/dev/null || { echo "error: qpdf checksum mismatch" >&2; exit 1; }
tar xzf "$tmp/qpdf.tar.gz" -C "$tmp"

# Shared library only, with qpdf's own (native) crypto: no OpenSSL or GnuTLS
# to bundle. Only the library and its headers are installed.
cmake -S "$tmp/qpdf-$QPDF_VERSION" -B "$tmp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_DOC=OFF \
  -DUSE_IMPLICIT_CRYPTO=OFF -DREQUIRE_CRYPTO_NATIVE=ON \
  -DINSTALL_EXAMPLES=OFF -DINSTALL_MANUAL=OFF -DINSTALL_PKGCONFIG=OFF
cmake --build "$tmp/build" --target libqpdf
rm -rf "$PREFIX"
for component in lib dev; do
  cmake --install "$tmp/build" --component "$component"
done
cp "$tmp/qpdf-$QPDF_VERSION/LICENSE.txt" "$PREFIX/LICENSE.txt" 2>/dev/null || true
echo "$QPDF_VERSION" > "$PREFIX/VERSION"
echo "qpdf $QPDF_VERSION installed in $PREFIX"
