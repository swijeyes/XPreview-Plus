// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PdfDocument.h"

#include "core/Limits.h"
#include "core/SafeImage.h"

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QMutex>

#include <cmath>
#include <mutex>

namespace {

std::recursive_mutex &pdfiumLock()
{
    static std::recursive_mutex m;
    return m;
}

constexpr int kMaxCachedPages = 24;

QString fromUtf16Buffer(const QVector<unsigned short> &buf)
{
    // Buffers are NUL-terminated UTF-16LE.
    int len = buf.size();
    while (len > 0 && buf[len - 1] == 0) --len;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(buf.constData()), len);
}

QString metaText(FPDF_DOCUMENT doc, const char *tag)
{
    const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= 2) return {};
    QVector<unsigned short> buf(bytes / 2 + 1);
    FPDF_GetMetaText(doc, tag, buf.data(), bytes);
    return fromUtf16Buffer(buf);
}

struct FileWriter : FPDF_FILEWRITE {
    QIODevice *file;
};

int writeBlock(FPDF_FILEWRITE *self, const void *data, unsigned long size)
{
    auto *w = static_cast<FileWriter *>(self);
    return w->file->write(static_cast<const char *>(data), qint64(size)) == qint64(size) ? 1 : 0;
}

}  // namespace

void PdfDocument::initLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        FPDF_InitLibraryWithConfig(&config);
    });
}

std::shared_ptr<PdfDocument> PdfDocument::open(const QString &path, const QByteArray &password,
                                               LoadError *error)
{
    initLibrary();
    std::lock_guard lock(pdfiumLock());
    const QByteArray utf8 = QFile::encodeName(path);
    FPDF_DOCUMENT doc =
        FPDF_LoadDocument(utf8.constData(), password.isEmpty() ? nullptr : password.constData());
    if (!doc) {
        switch (FPDF_GetLastError()) {
        case FPDF_ERR_FILE: *error = LoadError::File; break;
        case FPDF_ERR_FORMAT: *error = LoadError::Format; break;
        case FPDF_ERR_PASSWORD: *error = LoadError::Password; break;
        case FPDF_ERR_SECURITY: *error = LoadError::Security; break;
        default: *error = LoadError::Unknown; break;
        }
        return nullptr;
    }
    std::shared_ptr<PdfDocument> d(new PdfDocument);
    d->m_doc = doc;
    d->setFilePath(path);
    const int n = FPDF_GetPageCount(doc);
    d->m_pageSizes.reserve(n);
    for (int i = 0; i < n; ++i) {
        FS_SIZEF size{};
        const QSizeF letter(612, 792);
        if (FPDF_GetPageSizeByIndexF(doc, i, &size))
            d->m_pageSizes << SafeImage::sanitizePageSize(QSizeF(size.width, size.height), letter);
        else
            d->m_pageSizes << letter;
    }
    *error = LoadError::None;
    return d;
}

PdfDocument::~PdfDocument()
{
    std::lock_guard lock(pdfiumLock());
    releasePages();
    if (m_doc) FPDF_CloseDocument(m_doc);
}

void PdfDocument::releasePages()
{
    for (auto *t : std::as_const(m_textPages)) FPDFText_ClosePage(t);
    for (auto *p : std::as_const(m_pages)) FPDF_ClosePage(p);
    m_textPages.clear();
    m_pages.clear();
    m_pageOrder.clear();
}

QSizeF PdfDocument::pageSize(int page) const
{
    return page >= 0 && page < m_pageSizes.size() ? m_pageSizes[page] : QSizeF();
}

FPDF_PAGE PdfDocument::page(int index)
{
    if (index < 0 || index >= m_pageSizes.size()) return nullptr;
    m_pageOrder.removeOne(index);
    m_pageOrder.append(index);
    if (auto *p = m_pages.value(index)) return p;
    while (m_pageOrder.size() > kMaxCachedPages) {
        const int victim = m_pageOrder.takeFirst();
        if (auto *t = m_textPages.take(victim)) FPDFText_ClosePage(t);
        if (auto *p = m_pages.take(victim)) FPDF_ClosePage(p);
    }
    FPDF_PAGE p = FPDF_LoadPage(m_doc, index);
    if (p) m_pages.insert(index, p);
    return p;
}

FPDF_TEXTPAGE PdfDocument::textPage(int index)
{
    FPDF_PAGE p = page(index);
    if (!p) return nullptr;
    if (auto *t = m_textPages.value(index)) return t;
    FPDF_TEXTPAGE t = FPDFText_LoadPage(p);
    if (t) m_textPages.insert(index, t);
    return t;
}

QImage PdfDocument::renderPage(int index, double scale, const QRect &pixelRect)
{
    if (!renderSizeAllowed(pixelRect)) return {};
    QImage img(pixelRect.size(), QImage::Format_ARGB32);
    img.fill(Qt::white);
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return img;
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRA,
                                          img.bits(), int(img.bytesPerLine()));
    if (!bmp) return img;
    // Render the full page "device" offset so only the tile lands in the
    // bitmap. PDFium applies the page's /Rotate itself. QImage ARGB32 is
    // BGRA in memory on little-endian hosts, which matches FPDFBitmap_BGRA.
    const QSizeF size = m_pageSizes[index];
    const int fullW = qRound(size.width() * scale);
    const int fullH = qRound(size.height() * scale);
    FPDF_RenderPageBitmap(bmp, p, -pixelRect.x(), -pixelRect.y(), fullW, fullH, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bmp);
    return img;
}

void PdfDocument::paintPage(int index, QPainter *painter)
{
    // PDFium cannot paint into a QPainter, so print through a high
    // resolution raster. PDF export uses savePdfCopy() and stays vector.
    const QSizeF size = pageSize(index);
    // 300 dpi, reduced for very large pages so the bitmap stays bounded.
    const double scale = qMin(300.0 / 72.0, std::sqrt(double(Limits::kMaxRenderPixels) /
                                                      qMax(1.0, size.width() * size.height())));
    const QRect px(0, 0, qCeil(size.width() * scale), qCeil(size.height() * scale));
    const QImage img = renderPage(index, scale, px);
    painter->drawImage(QRectF(QPointF(0, 0), size), img);
}

QPointF PdfDocument::pageToPdf(int index, QPointF pt)
{
    FPDF_PAGE p = page(index);
    const QSizeF size = m_pageSizes[index];
    double x = 0, y = 0;
    FPDF_DeviceToPage(p, 0, 0, qRound(size.width() * 100), qRound(size.height() * 100), 0,
                      qRound(pt.x() * 100), qRound(pt.y() * 100), &x, &y);
    return {x, y};
}

QRectF PdfDocument::charBoxToPage(int index, double l, double t, double r, double b)
{
    FPDF_PAGE p = page(index);
    const QSizeF size = m_pageSizes[index];
    // Map through the device transform at 100x to keep sub-point precision.
    const int w = qRound(size.width() * 100), h = qRound(size.height() * 100);
    int x1, y1, x2, y2;
    FPDF_PageToDevice(p, 0, 0, w, h, 0, l, t, &x1, &y1);
    FPDF_PageToDevice(p, 0, 0, w, h, 0, r, b, &x2, &y2);
    return QRectF(QPointF(x1 / 100.0, y1 / 100.0), QPointF(x2 / 100.0, y2 / 100.0)).normalized();
}

QVector<PageRect> PdfDocument::rangeBoxes(int index, int start, int count)
{
    QVector<PageRect> out;
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return out;
    const int n = FPDFText_CountRects(t, start, count);
    for (int i = 0; i < n; ++i) {
        double l, top, r, b;
        if (FPDFText_GetRect(t, i, &l, &top, &r, &b))
            out << PageRect{index, charBoxToPage(index, l, top, r, b)};
    }
    return out;
}

QString PdfDocument::pageText(int index)
{
    std::lock_guard lock(pdfiumLock());
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return {};
    const int n = FPDFText_CountChars(t);
    if (n <= 0) return {};
    QVector<unsigned short> buf(n + 1);
    FPDFText_GetText(t, 0, n, buf.data());
    return fromUtf16Buffer(buf);
}

QVector<TextHit> PdfDocument::search(const QString &query, bool caseSensitive)
{
    QVector<TextHit> hits;
    if (query.isEmpty()) return hits;
    std::lock_guard lock(pdfiumLock());
    QVector<unsigned short> q(query.size() + 1, 0);
    memcpy(q.data(), query.utf16(), query.size() * sizeof(unsigned short));
    for (int i = 0; i < pageCount() && hits.size() < 5000; ++i) {
        FPDF_TEXTPAGE t = textPage(i);
        if (!t) continue;
        FPDF_SCHHANDLE h = FPDFText_FindStart(t, q.data(), caseSensitive ? FPDF_MATCHCASE : 0, 0);
        while (h && FPDFText_FindNext(h)) {
            const int start = FPDFText_GetSchResultIndex(h);
            const int count = FPDFText_GetSchCount(h);
            TextHit hit;
            hit.boxes = rangeBoxes(i, start, count);
            hit.text = query;
            if (!hit.boxes.isEmpty()) hits << hit;
        }
        if (h) FPDFText_FindClose(h);
    }
    return hits;
}

TextHit PdfDocument::selectText(int index, QPointF from, QPointF to)
{
    TextHit hit;
    std::lock_guard lock(pdfiumLock());
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return hit;
    const QPointF a = pageToPdf(index, from), b = pageToPdf(index, to);
    const double tol = 6.0;
    int i1 = FPDFText_GetCharIndexAtPos(t, a.x(), a.y(), tol, tol);
    int i2 = FPDFText_GetCharIndexAtPos(t, b.x(), b.y(), tol, tol);
    if (i1 < 0 || i2 < 0) {
        // Fall back to a rectangular selection when either end is off-text.
        const QRectF r = QRectF(a, b).normalized();
        const int len = FPDFText_GetBoundedText(t, r.left(), r.bottom(), r.right(), r.top(),
                                                nullptr, 0);
        if (len > 0) {
            QVector<unsigned short> buf(len + 1);
            FPDFText_GetBoundedText(t, r.left(), r.bottom(), r.right(), r.top(), buf.data(), len);
            hit.text = fromUtf16Buffer(buf);
            hit.boxes << PageRect{index, QRectF(from, to).normalized()};
        }
        return hit;
    }
    if (i1 > i2) std::swap(i1, i2);
    const int count = i2 - i1 + 1;
    QVector<unsigned short> buf(count + 1);
    FPDFText_GetText(t, i1, count, buf.data());
    hit.text = fromUtf16Buffer(buf);
    hit.boxes = rangeBoxes(index, i1, count);
    return hit;
}

QString PdfDocument::linkAt(int index, QPointF pt)
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return {};
    const QPointF pdf = pageToPdf(index, pt);
    FPDF_LINK link = FPDFLink_GetLinkAtPoint(p, pdf.x(), pdf.y());
    if (!link) return {};
    FPDF_ACTION action = FPDFLink_GetAction(link);
    if (action && FPDFAction_GetType(action) == PDFACTION_URI) {
        const unsigned long len = FPDFAction_GetURIPath(m_doc, action, nullptr, 0);
        QByteArray buf(int(len), 0);
        FPDFAction_GetURIPath(m_doc, action, buf.data(), len);
        return QString::fromUtf8(buf.constData());
    }
    FPDF_DEST dest = FPDFLink_GetDest(m_doc, link);
    if (!dest && action) dest = FPDFAction_GetDest(m_doc, action);
    if (dest) return QStringLiteral("#page=%1").arg(FPDFDest_GetDestPageIndex(m_doc, dest));
    return {};
}

static void readBookmarks(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, QList<OutlineItem> &out,
                          int depth, int &budget)
{
    if (depth > 32) return;
    for (FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, parent); bm && budget > 0;
         bm = FPDFBookmark_GetNextSibling(doc, bm)) {
        --budget;
        OutlineItem item;
        const unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
        QVector<unsigned short> buf(bytes / 2 + 1);
        FPDFBookmark_GetTitle(bm, buf.data(), bytes);
        item.title = fromUtf16Buffer(buf);
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
        if (!dest) {
            if (FPDF_ACTION a = FPDFBookmark_GetAction(bm)) dest = FPDFAction_GetDest(doc, a);
        }
        if (dest) item.page = FPDFDest_GetDestPageIndex(doc, dest);
        readBookmarks(doc, bm, item.children, depth + 1, budget);
        out << item;
    }
}

QList<OutlineItem> PdfDocument::outline()
{
    std::lock_guard lock(pdfiumLock());
    QList<OutlineItem> items;
    int budget = 20000;  // guard against cyclic outlines
    readBookmarks(m_doc, nullptr, items, 0, budget);
    return items;
}

QMap<QString, QString> PdfDocument::metadata()
{
    std::lock_guard lock(pdfiumLock());
    QMap<QString, QString> m;
    for (const char *tag : {"Title", "Author", "Subject", "Creator", "Producer", "CreationDate"}) {
        const QString v = metaText(m_doc, tag);
        if (!v.isEmpty()) m.insert(QString::fromLatin1(tag), v);
    }
    int version = 0;
    if (FPDF_GetFileVersion(m_doc, &version))
        m.insert(QStringLiteral("PDF version"), QStringLiteral("%1.%2").arg(version / 10).arg(version % 10));
    m.insert(QStringLiteral("Pages"), QString::number(pageCount()));
    return m;
}

bool PdfDocument::savePdfCopy(const QString &path, int extraRotation, QString *error)
{
    extraRotation = ((extraRotation % 360) + 360) % 360;
    // Writes go to a temporary file that atomically replaces the target, so
    // a failure never leaves a partial PDF and the source file (which
    // PDFium may still be reading) is never truncated in place.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    if (extraRotation == 0) {
        // Lossless byte copy.
        if (QFileInfo(path).canonicalFilePath() == QFileInfo(filePath()).canonicalFilePath()) {
            out.cancelWriting();
            return true;  // exporting a PDF onto itself: nothing to do
        }
        QFile in(filePath());
        bool ok = in.open(QIODevice::ReadOnly);
        while (ok && !in.atEnd()) {
            const QByteArray chunk = in.read(1 << 20);
            ok = !chunk.isEmpty() && out.write(chunk) == chunk.size();
        }
        if (ok && out.commit()) return true;
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    std::lock_guard lock(pdfiumLock());
    QVector<int> original;
    if (extraRotation) {
        for (int i = 0; i < pageCount(); ++i) {
            FPDF_PAGE p = page(i);
            const int r = FPDFPage_GetRotation(p);
            original << r;
            FPDFPage_SetRotation(p, (r + extraRotation / 90) % 4);
        }
    }
    FileWriter w{};
    w.version = 1;
    w.WriteBlock = writeBlock;
    w.file = &out;
    bool ok = FPDF_SaveAsCopy(m_doc, &w, FPDF_NO_INCREMENTAL);
    for (int i = 0; i < original.size(); ++i) FPDFPage_SetRotation(page(i), original[i]);
    ok = ok && out.commit();
    if (!ok && error) *error = QStringLiteral("Could not write %1").arg(path);
    return ok;
}
