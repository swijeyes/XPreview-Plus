// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "Exporter.h"

#include <QFileInfo>
#include <QPdfWriter>
#include <QPrinter>
#include <QSaveFile>

namespace Exporter {

QSizeF rotatedSize(QSizeF size, int rotation)
{
    rotation = ((rotation % 360) + 360) % 360;
    return rotation == 90 || rotation == 270 ? size.transposed() : size;
}

void applyRotation(QPainter *painter, QSizeF s, int rotation)
{
    rotation = ((rotation % 360) + 360) % 360;
    switch (rotation) {
    case 90: painter->translate(s.height(), 0); break;
    case 180: painter->translate(s.width(), s.height()); break;
    case 270: painter->translate(0, s.width()); break;
    default: break;
    }
    painter->rotate(rotation);
}

bool exportPdf(Document &doc, const QString &path, int rotation, QString *error)
{
    if (!doc.nativePdfPath().isEmpty()) return doc.savePdfCopy(path, rotation, error);

    // Atomic write: a failed export never leaves a partial file behind.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    QPdfWriter writer(&file);
    writer.setResolution(72);  // user space == points
    writer.setCreator(QStringLiteral(XPREVIEW_DISPLAY_NAME " " XPREVIEW_VERSION));
    const QMap<QString, QString> meta = doc.metadata();
    writer.setTitle(meta.value(QStringLiteral("Title"), QFileInfo(doc.filePath()).completeBaseName()));
    writer.setPdfVersion(QPagedPaintDevice::PdfVersion_1_6);

    auto setPage = [&](int i) {
        const QSizeF s = rotatedSize(doc.pageSize(i), rotation);
        writer.setPageSize(QPageSize(s, QPageSize::Point, QString(), QPageSize::ExactMatch));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    };
    setPage(0);
    QPainter painter;
    if (!painter.begin(&writer)) {
        file.cancelWriting();
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    for (int i = 0; i < doc.pageCount(); ++i) {
        if (i > 0) {
            setPage(i);
            writer.newPage();
        }
        painter.save();
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                               QPainter::SmoothPixmapTransform);
        applyRotation(&painter, doc.pageSize(i), rotation);
        painter.setClipRect(QRectF(QPointF(0, 0), doc.pageSize(i)));
        painter.fillRect(QRectF(QPointF(0, 0), doc.pageSize(i)), Qt::white);
        doc.paintPage(i, &painter);
        painter.restore();
    }
    if (!painter.end() || !file.commit()) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    return true;
}

void print(Document &doc, QPrinter *printer, int rotation)
{
    QPainter painter;
    if (!painter.begin(printer)) return;
    int from = printer->fromPage(), to = printer->toPage();
    if (from <= 0) from = 1;
    if (to <= 0 || to > doc.pageCount()) to = doc.pageCount();
    bool first = true;
    for (int i = from - 1; i < to; ++i) {
        if (!first) printer->newPage();
        first = false;
        const QRectF paper = printer->pageRect(QPrinter::DevicePixel);
        const QSizeF size = rotatedSize(doc.pageSize(i), rotation);
        const double scale = qMin(paper.width() / size.width(), paper.height() / size.height());
        painter.save();
        painter.translate((paper.width() - size.width() * scale) / 2,
                          (paper.height() - size.height() * scale) / 2);
        painter.scale(scale, scale);
        applyRotation(&painter, doc.pageSize(i), rotation);
        painter.setClipRect(QRectF(QPointF(0, 0), doc.pageSize(i)));
        doc.paintPage(i, &painter);
        painter.restore();
    }
    painter.end();
}

}  // namespace Exporter
