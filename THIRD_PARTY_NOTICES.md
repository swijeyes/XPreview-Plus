# Third-party notices

XPreview+ is licensed under the **GNU Affero General Public License v3.0 or
later** (`LICENSE`). It uses the components below. Each one's license is
compatible with distribution under the AGPLv3, and their full license texts are
in `licenses/` (and in the app bundle or zip).

## Linked into, or shipped with, every build

| Component | Use | License | Compatibility |
|---|---|---|---|
| [Qt 6](https://www.qt.io) (qtbase, qtsvg, qtimageformats) | UI, layout, images, SVG, printing | LGPL-3.0-only | Compatible with AGPLv3 (LGPLv3 is GPLv3 plus extra permissions). Linked dynamically and replaceable by the user. Source: <https://download.qt.io/official_releases/qt/>. Texts: `licenses/LGPL-3.0.txt`, `licenses/GPL-3.0.txt` |
| [PDFium](https://pdfium.googlesource.com/pdfium/) via [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) | PDF rendering, text, search | BSD-3-Clause / Apache-2.0 | Compatible. `licenses/pdfium/` |
| PDFium's bundled libraries: Abseil, AGG 2.3, dragonbox, fast_float, FreeType, HarfBuzz, Highway, ICU, Little CMS, libjpeg-turbo, OpenJPEG, libpng, LLVM libc, simdutf, zlib | (inside `pdfium.dll` / `libpdfium.dylib`) | Apache-2.0, BSD, MIT, FTL, IJG, Unicode, zlib, Boost | All permissive; compatible. `licenses/pdfium/` |
| [miniz](https://github.com/richgel999/miniz) | ODF (ZIP) reading | MIT | Compatible. `licenses/miniz.txt` |
| Arimo, Tinos, Cousine (ChromeOS core fonts 1.31) | Metric-compatible substitutes for Arial, Times New Roman and Courier New, and same-design stand-ins for Liberation Sans/Serif/Mono | Apache-2.0 | Compatible. Shipped unmodified. `licenses/Apache-2.0.txt` |
| [Carlito](https://github.com/googlefonts/carlito), [Caladea](https://github.com/huertatipografica/Caladea) | Substitutes for Calibri, Cambria | SIL OFL 1.1 | Shipped unmodified as separate font files (an aggregate, not combined with the code). `licenses/LICENSE-Carlito.txt`, `licenses/LICENSE-Caladea.txt` |

## Windows build only

| Component | License | Notes |
|---|---|---|
| llvm-mingw runtime (`libc++.dll`, `libunwind.dll`) | Apache-2.0 WITH LLVM-exception | `licenses/llvm-mingw.txt` |
| mingw-w64 runtime (startup code, statically linked) | ZPL-2.1 / MIT / public domain | `licenses/mingw-w64-runtime.txt` |

## macOS build only

Homebrew's Qt is built against these libraries, and `macdeployqt` copies them
into `XPreview+.app/Contents/Frameworks`. The packaging script copies each
one's license file into `Contents/Resources/licenses/<package>/`.

| Library | License |
|---|---|
| brotli, harfbuzz, md4c | MIT |
| double-conversion, pcre2, libwebp/sharpyuv, libtiff, libpng, libjpeg-turbo | BSD-style / IJG / libpng |
| zstd | BSD-3-Clause (dual GPL-2.0) |
| xz (liblzma) | 0BSD |
| libb2 | CC0-1.0 |
| OpenSSL (3.x/4.x) | Apache-2.0 |
| ICU | Unicode-3.0 |
| FreeType | FTL (dual GPL-2.0-or-later) |
| glib, gettext (libintl), graphite2 | LGPL-2.1-or-later (`licenses/LGPL-2.1.txt`) |
| D-Bus | AFL-2.1 OR GPL-2.0-or-later (used under GPL-2.0-or-later) |

## Artwork

The app icon (`icon/`, `resources/XPreview.icns`, `.ico`, `icon-1024.png`) was
created by Sanjeeva Wijeyesakere using Google Gemini (the "nano-banana" image
model) and is distributed with XPreview+ under the same AGPL-3.0-or-later
terms. Google's Gemini terms do not claim ownership of generated images.

## Not included

PDFium is used instead of MuPDF (AGPL) or Poppler (GPL) by design (see
`plan/`). LibreOffice is not used. No code from Apple's Preview is used.
