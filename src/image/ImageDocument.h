// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QSvgRenderer>

// Raster images via Qt image plugins. Multi-page TIFFs (and other multi-frame
// files that are not animations) become pages. EXIF orientation and ICC
// profiles are applied on load.
class ImageDocument : public Document {
public:
    static std::shared_ptr<ImageDocument> open(const QString &path, QString *error);

    DocumentKind kind() const override { return DocumentKind::Image; }
    int pageCount() const override { return m_frames.size(); }
    QSizeF pageSize(int page) const override;
    QImage renderPage(int page, double scale, const QRect &pixelRect) override;
    void paintPage(int page, QPainter *painter) override;
    QMap<QString, QString> metadata() override { return m_meta; }

    const QImage &frame(int page) const { return m_frames[page].levels.first(); }

    // When true, pages report their printed size (honouring the file's DPI);
    // when false, one image pixel maps to one point. Changes the reported
    // pageSize(), so the view must relayout afterwards.
    void setPrintedSize(bool on) { m_printedSize = on; }

private:
    struct Frame {
        QList<QImage> levels;  // [0] full size, then successive halvings
        QSizeF pixelSizePt;    // one point per image pixel (ignores DPI)
        QSizeF printSizePt;    // printed size, honouring the file's DPI
    };
    const QImage &levelFor(int page, double pixelsPerImagePixel);
    QSizeF sizePt(int page) const { return m_printedSize ? m_frames[page].printSizePt : m_frames[page].pixelSizePt; }

    QVector<Frame> m_frames;
    QMap<QString, QString> m_meta;
    bool m_printedSize = false;
};

class SvgDocument : public Document {
public:
    static std::shared_ptr<SvgDocument> open(const QString &path, QString *error);

    DocumentKind kind() const override { return DocumentKind::Svg; }
    int pageCount() const override { return 1; }
    QSizeF pageSize(int) const override { return m_size; }
    void paintPage(int page, QPainter *painter) override;

private:
    QByteArray m_data;
    QSizeF m_size;
};
