// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QByteArray>
#include <QImage>
#include <QSizeF>

#include <cstdint>

// Untrusted-image helpers shared by every backend.
namespace SafeImage {

// One-time process setup: global allocation limit, worker stack sizes.
// Idempotent and thread-safe.
void initProcessLimits();

// Neutralises SVG references to anything outside the document itself
// (href/xlink:href that is not "#fragment" or "data:"), so QtSvg never
// reads local files. Handles gzip-compressed SVG (svgz). Returns an empty
// array if the input is not usable.
QByteArray sanitizeSvg(const QByteArray &data);
bool looksLikeSvg(const QByteArray &data);

// Decoded-pixel budget for all images embedded in one document.
class Budget {
public:
    explicit Budget(std::int64_t bytes);
    bool take(std::int64_t bytes);
    bool exhausted() const { return m_left <= 0; }

private:
    std::int64_t m_left;
};

// Decodes an image embedded in a document (RTF \pict, ODF Pictures/*),
// checking dimensions before allocation and charging the budget. Returns a
// null image when refused or undecodable.
QImage decodeEmbedded(const QByteArray &data, Budget &budget, bool *refused = nullptr);

// Clamps a page size from a file to sane, finite bounds.
QSizeF sanitizePageSize(QSizeF size, QSizeF fallback);
// Clamps any length from a file to [-kMaxLength, kMaxLength]; non-finite -> 0.
double clampLength(double v);

}  // namespace SafeImage
