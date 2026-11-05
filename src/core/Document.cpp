// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "Document.h"

#include "Limits.h"

QString documentKindName(DocumentKind kind)
{
    switch (kind) {
    case DocumentKind::Pdf: return QStringLiteral("PDF document");
    case DocumentKind::Image: return QStringLiteral("Image");
    case DocumentKind::Svg: return QStringLiteral("SVG image");
    case DocumentKind::Rtf: return QStringLiteral("RTF document");
    case DocumentKind::Odt: return QStringLiteral("OpenDocument text");
    }
    return {};
}

bool Document::renderSizeAllowed(const QRect &pixelRect)
{
    return !pixelRect.isEmpty() &&
           qint64(pixelRect.width()) * pixelRect.height() <= Limits::kMaxRenderPixels;
}

QImage Document::renderPage(int page, double scale, const QRect &pixelRect)
{
    if (!renderSizeAllowed(pixelRect)) return {};
    QImage img(pixelRect.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);
    p.translate(-pixelRect.topLeft());
    p.scale(scale, scale);
    p.setClipRect(QRectF(QPointF(0, 0), pageSize(page)));
    paintPage(page, &p);
    return img;
}
