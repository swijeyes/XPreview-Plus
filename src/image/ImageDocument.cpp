// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "ImageDocument.h"

#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

namespace {

QImage normalise(QImage img)
{
    if (img.colorSpace().isValid() && img.colorSpace() != QColorSpace::SRgb)
        img.convertToColorSpace(QColorSpace::SRgb);
    if (img.format() != QImage::Format_ARGB32_Premultiplied &&
        img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                        : QImage::Format_RGB32);
    }
    return img;
}

QSizeF sizeInPoints(const QImage &img)
{
    // Respect the file's DPI when it declares one, otherwise 72 dpi so one
    // image pixel is one point at 100%.
    double dpiX = img.dotsPerMeterX() * 0.0254, dpiY = img.dotsPerMeterY() * 0.0254;
    if (dpiX < 30 || dpiX > 1200) dpiX = 72;
    if (dpiY < 30 || dpiY > 1200) dpiY = 72;
    return QSizeF(img.width() * 72.0 / dpiX, img.height() * 72.0 / dpiY);
}

}  // namespace

std::shared_ptr<ImageDocument> ImageDocument::open(const QString &path, QString *error)
{
    SafeImage::initProcessLimits();
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);  // EXIF orientation
    const QSize sz = reader.size();
    if (sz.isValid() &&
        (sz.width() > Limits::kMaxImageDimension || sz.height() > Limits::kMaxImageDimension)) {
        *error = QStringLiteral("Image is too large (%1 × %2)").arg(sz.width()).arg(sz.height());
        return nullptr;
    }

    if (reader.format().startsWith("svg")) {
        *error = QStringLiteral("SVG must be opened as a vector document");
        return nullptr;
    }
    std::shared_ptr<ImageDocument> d(new ImageDocument);
    d->setFilePath(path);
    // Animated formats show their first frame; multi-page TIFF shows pages.
    const bool pagesFormat = reader.format() == "tiff" || reader.format() == "tif";
    const int count = pagesFormat ? qBound(1, reader.imageCount(), 2000) : 1;
    // Multi-page files share one decoded-memory budget.
    SafeImage::Budget budget(Limits::kMaxDocumentImageBytes);
    for (int i = 0; i < count; ++i) {
        if (i > 0 && !reader.jumpToNextImage()) break;
        QImage img = reader.read();
        if (img.isNull()) {
            if (i == 0) {
                *error = reader.errorString();
                return nullptr;
            }
            break;
        }
        if (!budget.take(img.sizeInBytes()) && i > 0) {
            d->m_report.note(QStringLiteral("Some pages were not loaded (file too large)"));
            break;
        }
        Frame f;
        const QSizeF pixels(img.width(), img.height());
        f.pixelSizePt = SafeImage::sanitizePageSize(pixels, pixels);
        f.printSizePt = SafeImage::sanitizePageSize(sizeInPoints(img), pixels);
        f.levels << normalise(std::move(img));
        d->m_frames << f;
    }

    const QImage &first = d->m_frames.first().levels.first();
    d->m_meta.insert(QStringLiteral("Format"), QString::fromLatin1(reader.format()).toUpper());
    d->m_meta.insert(QStringLiteral("Dimensions"),
                     QStringLiteral("%1 × %2 pixels").arg(first.width()).arg(first.height()));
    if (first.colorSpace().isValid())
        d->m_meta.insert(QStringLiteral("Color profile"), first.colorSpace().description());
    for (const QString &key : reader.textKeys()) {
        const QString v = reader.text(key);
        if (!v.isEmpty() && v.size() < 500) d->m_meta.insert(key, v);
    }
    if (reader.supportsAnimation() && reader.imageCount() > 1)
        d->m_report.note(QStringLiteral("Animated image: showing the first frame"));
    return d;
}

QSizeF ImageDocument::pageSize(int page) const
{
    return page >= 0 && page < m_frames.size() ? sizePt(page) : QSizeF();
}

const QImage &ImageDocument::levelFor(int page, double pixelsPerImagePixel)
{
    // Mip levels avoid aliasing when drawing big images small (thumbnails).
    Frame &f = m_frames[page];
    int level = 0;
    double s = 1.0;
    while (pixelsPerImagePixel < s * 0.5) {
        ++level;
        s *= 0.5;
        if (level >= f.levels.size()) {
            const QImage &prev = f.levels.last();
            if (prev.width() < 64 || prev.height() < 64) { --level; break; }
            f.levels << prev.scaled(prev.width() / 2, prev.height() / 2, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
        }
    }
    return f.levels[level];
}

QImage ImageDocument::renderPage(int page, double scale, const QRect &pixelRect)
{
    if (!renderSizeAllowed(pixelRect)) return {};
    QImage out(pixelRect.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::white);
    std::lock_guard lock(mutex());
    if (page < 0 || page >= m_frames.size()) return out;
    const QImage &full = m_frames[page].levels.first();
    const QSizeF sz = sizePt(page);
    const double pxPerImgPx = scale * sz.width() / full.width();
    const QImage &src = levelFor(page, pxPerImgPx);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, pxPerImgPx < 3.0);
    const QRectF target(-pixelRect.x(), -pixelRect.y(), sz.width() * scale, sz.height() * scale);
    // Only the source region under the tile is sampled.
    const double sx = src.width() / target.width(), sy = src.height() / target.height();
    const QRectF tile(0, 0, pixelRect.width(), pixelRect.height());
    const QRectF visible = tile.intersected(target);
    const QRectF source((visible.x() - target.x()) * sx, (visible.y() - target.y()) * sy,
                        visible.width() * sx, visible.height() * sy);
    p.drawImage(visible, src, source);
    return out;
}

void ImageDocument::paintPage(int page, QPainter *painter)
{
    std::lock_guard lock(mutex());
    if (page < 0 || page >= m_frames.size()) return;
    painter->drawImage(QRectF(QPointF(0, 0), sizePt(page)), m_frames[page].levels.first());
}

std::shared_ptr<SvgDocument> SvgDocument::open(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return nullptr;
    }
    std::shared_ptr<SvgDocument> d(new SvgDocument);
    d->setFilePath(path);
    if (f.size() > Limits::kMaxSvgBytes) {
        *error = QStringLiteral("The SVG file is too large");
        return nullptr;
    }
    // Strip references to external files before QtSvg sees the data.
    d->m_data = SafeImage::sanitizeSvg(f.read(Limits::kMaxSvgBytes));
    QSvgRenderer r(d->m_data);
    if (!r.isValid()) {
        *error = QStringLiteral("Invalid SVG file");
        return nullptr;
    }
    QSizeF size = r.defaultSize();
    if (size.isEmpty()) size = r.viewBoxF().size();
    d->m_size = SafeImage::sanitizePageSize(size, QSizeF(512, 512));
    return d;
}

void SvgDocument::paintPage(int, QPainter *painter)
{
    std::lock_guard lock(mutex());
    QSvgRenderer r(m_data);  // renderer per call: cheap and thread-confined
    r.render(painter, QRectF(QPointF(0, 0), m_size));
}
