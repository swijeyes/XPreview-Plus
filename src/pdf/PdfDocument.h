// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QHash>

struct fpdf_document_t__;
struct fpdf_page_t__;
struct fpdf_textpage_t__;

// PDFium-backed PDF. PDFium is not thread-safe, so every call into it goes
// through one process-wide lock (pdfiumLock()).
class PdfDocument : public Document {
public:
    enum class LoadError { None, File, Format, Password, Security, Unknown };

    static std::shared_ptr<PdfDocument> open(const QString &path, const QByteArray &password,
                                             LoadError *error);
    ~PdfDocument() override;

    DocumentKind kind() const override { return DocumentKind::Pdf; }
    int pageCount() const override { return m_pageSizes.size(); }
    QSizeF pageSize(int page) const override;
    QImage renderPage(int page, double scale, const QRect &pixelRect) override;
    void paintPage(int page, QPainter *painter) override;

    bool hasText() const override { return true; }
    QString pageText(int page) override;
    QVector<TextHit> search(const QString &query, bool caseSensitive) override;
    TextHit selectText(int page, QPointF from, QPointF to) override;
    QString linkAt(int page, QPointF pt) override;

    QList<OutlineItem> outline() override;
    QMap<QString, QString> metadata() override;

    QString nativePdfPath() const override { return filePath(); }
    bool savePdfCopy(const QString &path, int extraRotation, QString *error) override;

    static void initLibrary();

private:
    PdfDocument() = default;
    fpdf_page_t__ *page(int index);
    fpdf_textpage_t__ *textPage(int index);
    void releasePages();
    QRectF charBoxToPage(int page, double l, double t, double r, double b);
    QPointF pageToPdf(int page, QPointF pt);
    QVector<PageRect> rangeBoxes(int page, int start, int count);

    fpdf_document_t__ *m_doc = nullptr;
    QVector<QSizeF> m_pageSizes;
    // Small LRU of loaded pages; PDFium pages are expensive to keep forever.
    QHash<int, fpdf_page_t__ *> m_pages;
    QHash<int, fpdf_textpage_t__ *> m_textPages;
    QList<int> m_pageOrder;
};
