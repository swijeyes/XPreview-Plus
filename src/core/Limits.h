// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <cstdint>

// Hard limits that keep hostile or pathological files bounded (plan section 9).
namespace Limits {

// Input files
constexpr std::int64_t kMaxTextFileBytes = 512ll * 1024 * 1024;  // RTF / FODT / SVG

// ZIP / ODF package
constexpr int kMaxZipEntries = 20000;
constexpr std::uint64_t kMaxEntryBytes = 256ull * 1024 * 1024;
constexpr std::uint64_t kMaxTotalBytes = 512ull * 1024 * 1024;
constexpr double kMaxCompressionRatio = 1000.0;

// XML
constexpr int kMaxXmlDepth = 128;
constexpr int kMaxTableRows = 100000;
constexpr int kMaxTableColumns = 1024;
constexpr int kMaxTableCells = 250000;  // per document, all tables
constexpr int kMaxRepeatEmpty = 1;      // empty repeated rows/columns collapse
constexpr int kMaxRepeat = 1000;        // repeated non-empty rows/columns
constexpr int kMaxTabStops = 64;

// RTF
constexpr int kMaxRtfGroupDepth = 1000;
constexpr std::int64_t kMaxRtfTokens = 100'000'000;
constexpr std::int64_t kMaxRtfBinBytes = 64ll * 1024 * 1024;
constexpr std::int64_t kMaxPictBytes = 64ll * 1024 * 1024;
constexpr int kMaxListLevels = 9;

// Images
constexpr int kMaxImageDimension = 30000;
constexpr int kImageAllocationLimitMB = 1024;          // one decoded image
constexpr std::int64_t kMaxEmbeddedImagePixels = 40'000'000;   // one image in RTF/ODT
constexpr std::int64_t kMaxDocumentImageBytes = 768ll * 1024 * 1024;  // all images in a document
constexpr std::int64_t kMaxSvgBytes = 64ll * 1024 * 1024;

// Geometry (points). Sanitised everywhere a file supplies a length.
constexpr double kMaxPageSide = 14400.0 * 4;  // 800 inches
constexpr double kMinPageSide = 1.0;
constexpr double kMaxLength = 14400.0;        // indents, spacing, widths
constexpr double kMaxFontSize = 1638.0;

// Rendering
constexpr std::int64_t kMaxRenderPixels = 64ll * 1024 * 1024;  // one bitmap
constexpr int kMaxThumbnailAspect = 4;                         // height <= 4 x width
constexpr double kMaxContentPixels = 1.5e9;                    // scroll range safety

// Worker threads parse deep (but bounded) structures recursively.
constexpr unsigned kWorkerStackBytes = 8u * 1024 * 1024;

}  // namespace Limits
