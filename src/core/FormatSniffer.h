// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QString>

enum class SniffedFormat {
    Unknown,
    Pdf,
    Rtf,
    OdfText,          // .odt / .ott (zip package)
    OdfSpreadsheet,   // recognised but not supported in this build
    OdfPresentation,  // recognised but not supported in this build
    FlatOdfText,      // .fodt
    Svg,
    Image,
};

struct SniffResult {
    SniffedFormat format = SniffedFormat::Unknown;
    bool extensionMismatch = false;  // content disagrees with the extension
    QString description;
};

// Content-based detection; extensions are only a tie-breaker (plan 3.2).
SniffResult sniffFile(const QString &path);
