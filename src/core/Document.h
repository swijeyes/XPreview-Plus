// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "FidelityReport.h"

#include <QImage>
#include <QList>
#include <QMap>
#include <QPainter>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <memory>
#include <mutex>

// A rectangle on a page, in page points (1/72 inch, origin top-left).
struct PageRect {
    int page = 0;
    QRectF rect;
};

// A run of text found by search or selected by the user.
struct TextHit {
    QVector<PageRect> boxes;
    QString text;
    int page() const { return boxes.isEmpty() ? -1 : boxes.first().page; }
};

struct OutlineItem {
    QString title;
    int page = -1;
    double y = 0;  // position on the page in points, if known
    QList<OutlineItem> children;
};

enum class DocumentKind { Pdf, Image, Svg, Rtf, Odt };

QString documentKindName(DocumentKind kind);

// Format-agnostic document interface. Everything is pages: an image is a
// one-page document, a flowing text document is paginated, so the viewer,
// thumbnails, print and export are written once.
//
// Thread safety: implementations must allow renderPage() to be called from
// worker threads concurrently with GUI-thread calls. Each implementation
// serialises access to its internal state through mutex().
class Document {
public:
    virtual ~Document() = default;

    virtual DocumentKind kind() const = 0;
    virtual int pageCount() const = 0;
    virtual QSizeF pageSize(int page) const = 0;  // points

    // Raster render of `pixelRect` (device pixels) of the page at `scale`
    // pixels per point. The default paints through paintPage().
    virtual QImage renderPage(int page, double scale, const QRect &pixelRect);

    // Vector paint of the page into a painter whose user space is page
    // points. Used for print and PDF export.
    virtual void paintPage(int page, QPainter *painter) = 0;

    virtual QString pageText(int page) { Q_UNUSED(page); return {}; }
    virtual QVector<TextHit> search(const QString &query, bool caseSensitive) {
        Q_UNUSED(query); Q_UNUSED(caseSensitive); return {};
    }
    // Stream selection between two points on the same page.
    virtual TextHit selectText(int page, QPointF from, QPointF to) {
        Q_UNUSED(page); Q_UNUSED(from); Q_UNUSED(to); return {};
    }
    virtual bool hasText() const { return false; }

    // Link under a point (external URL), empty if none.
    virtual QString linkAt(int page, QPointF pt) { Q_UNUSED(page); Q_UNUSED(pt); return {}; }

    virtual QList<OutlineItem> outline() { return {}; }
    virtual QMap<QString, QString> metadata() { return {}; }
    const FidelityReport &fidelityReport() const { return m_report; }

    // A PDF on disk that represents this document losslessly, if any.
    virtual QString nativePdfPath() const { return {}; }
    // Lossless PDF copy with extra rotation (degrees, multiple of 90).
    virtual bool savePdfCopy(const QString &path, int extraRotation, QString *error) {
        Q_UNUSED(path); Q_UNUSED(extraRotation);
        if (error) *error = QStringLiteral("Not a PDF document");
        return false;
    }

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString &p) { m_filePath = p; }

protected:
    // Refuses bitmaps whose size would be unreasonable (hostile page sizes).
    static bool renderSizeAllowed(const QRect &pixelRect);
    std::recursive_mutex &mutex() const { return m_mutex; }
    FidelityReport m_report;

private:
    QString m_filePath;
    mutable std::recursive_mutex m_mutex;
};

using DocumentPtr = std::shared_ptr<Document>;
