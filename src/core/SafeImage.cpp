// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "SafeImage.h"

#include "Limits.h"

#include <QBuffer>
#include <QImageReader>
#include <QRegularExpression>
#include <QThreadPool>

#include <miniz.h>

#include <cmath>
#include <mutex>

namespace SafeImage {

void initProcessLimits()
{
    static std::once_flag once;
    std::call_once(once, [] {
        QImageReader::setAllocationLimit(Limits::kImageAllocationLimitMB);
        // macOS secondary threads default to 512 KB stacks.
        QThreadPool::globalInstance()->setStackSize(Limits::kWorkerStackBytes);
    });
}

namespace {

// Minimal gzip reader (RFC 1952) on top of miniz's raw inflate, capped.
QByteArray gunzip(const QByteArray &in)
{
    const auto *d = reinterpret_cast<const unsigned char *>(in.constData());
    const qsizetype n = in.size();
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) return {};
    const int flags = d[3];
    qsizetype pos = 10;
    if (flags & 4) {  // FEXTRA
        if (pos + 2 > n) return {};
        pos += 2 + (d[pos] | (d[pos + 1] << 8));
    }
    if (flags & 8) while (pos < n && d[pos++]) {}   // FNAME
    if (flags & 16) while (pos < n && d[pos++]) {}  // FCOMMENT
    if (flags & 2) pos += 2;                        // FHCRC
    if (pos >= n - 8) return {};
    size_t outLen = 0;
    void *out = tinfl_decompress_mem_to_heap(d + pos, size_t(n - 8 - pos), &outLen, 0);
    if (!out) return {};
    QByteArray result;
    if (outLen <= size_t(Limits::kMaxSvgBytes)) result = QByteArray(static_cast<const char *>(out), qsizetype(outLen));
    mz_free(out);
    return result;
}

}  // namespace

bool looksLikeSvg(const QByteArray &data)
{
    if (data.startsWith("\x1f\x8b")) return true;  // svgz; validated on sanitise
    const QByteArray head = data.left(4096);
    return head.contains("<svg") || head.contains("<!DOCTYPE svg");
}

QByteArray sanitizeSvg(const QByteArray &input)
{
    QByteArray data = input.startsWith("\x1f\x8b") ? gunzip(input) : input;
    if (data.isEmpty() || data.size() > Limits::kMaxSvgBytes) return {};
    // Any href that is not an in-document fragment or inline data is
    // replaced, so <image>, <use> and friends cannot reach the filesystem.
    static const QRegularExpression href(
        QStringLiteral(R"(((?:xlink:)?href\s*=\s*)(["'])\s*(?!#|data:)[^"']*\2)"),
        QRegularExpression::CaseInsensitiveOption);
    QString text = QString::fromUtf8(data);
    text.replace(href, QStringLiteral("\\1\\2#blocked\\2"));
    // External entities / DTD subsets are not needed to render SVG.
    static const QRegularExpression doctype(QStringLiteral(R"(<!DOCTYPE[^\[>]*(\[[^\]]*\])?\s*>)"),
                                            QRegularExpression::CaseInsensitiveOption);
    text.remove(doctype);
    return text.toUtf8();
}

Budget::Budget(std::int64_t bytes) : m_left(bytes) {}

bool Budget::take(std::int64_t bytes)
{
    if (bytes > m_left) return false;
    m_left -= bytes;
    return true;
}

QImage decodeEmbedded(const QByteArray &input, Budget &budget, bool *refused)
{
    if (refused) *refused = false;
    QByteArray data = input;
    if (looksLikeSvg(data)) {
        data = sanitizeSvg(data);
        if (data.isEmpty()) return {};
    }
    QBuffer buf(&data);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    const QSize sz = reader.size();
    // Unknown dimensions are refused: we must know the cost up front.
    if (!sz.isValid() || sz.width() <= 0 || sz.height() <= 0 || sz.width() > Limits::kMaxImageDimension ||
        sz.height() > Limits::kMaxImageDimension ||
        std::int64_t(sz.width()) * sz.height() > Limits::kMaxEmbeddedImagePixels ||
        !budget.take(std::int64_t(sz.width()) * sz.height() * 4)) {
        if (refused) *refused = true;
        return {};
    }
    QImage img = reader.read();
    // A decoder may still produce something larger than it announced.
    if (!img.isNull() && (img.width() > sz.width() * 2 || img.height() > sz.height() * 2)) return {};
    return img;
}

double clampLength(double v)
{
    if (!std::isfinite(v)) return 0;
    return qBound(-Limits::kMaxLength, v, Limits::kMaxLength);
}

QSizeF sanitizePageSize(QSizeF size, QSizeF fallback)
{
    double w = size.width(), h = size.height();
    if (!std::isfinite(w) || !std::isfinite(h) || w < Limits::kMinPageSide || h < Limits::kMinPageSide)
        return fallback;
    return QSizeF(qMin(w, Limits::kMaxPageSide), qMin(h, Limits::kMaxPageSide));
}

}  // namespace SafeImage
