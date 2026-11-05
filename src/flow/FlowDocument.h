// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "FlowBuilder.h"
#include "core/Document.h"

#include <QTextDocument>

// A flowing text document (ODT or RTF) laid out by QTextDocument and cut into
// pages. Headers and footers are separate documents painted in the margins,
// with page-number fields substituted per page.
class FlowDocument : public Document {
public:
    FlowDocument(DocumentKind kind, flow::Document model, const QString &path);
    ~FlowDocument() override;

    DocumentKind kind() const override { return m_kind; }
    int pageCount() const override { return m_pageCount; }
    QSizeF pageSize(int) const override { return m_model.page.size; }
    void paintPage(int page, QPainter *painter) override;

    bool hasText() const override { return true; }
    QString pageText(int page) override;
    QVector<TextHit> search(const QString &query, bool caseSensitive) override;
    TextHit selectText(int page, QPointF from, QPointF to) override;
    QString linkAt(int page, QPointF pt) override;
    QList<OutlineItem> outline() override;
    QMap<QString, QString> metadata() override { return m_model.metadata; }

    // Called once after construction on the GUI thread (QTextDocument and
    // its layout are QObjects).
    void moveToThread(QThread *thread);

private:
    QVector<PageRect> rangeBoxes(int from, int to);
    QPointF toDocPoint(int page, QPointF pt) const;
    std::unique_ptr<QTextDocument> buildHeaderFooter(const flow::Blocks &blocks, int page);
    const flow::Blocks &headerFor(int page) const;
    const flow::Blocks &footerFor(int page) const;
    double blocksHeight(const flow::Blocks &blocks);
    void paintHeaderFooter(int page, QPainter *painter);

    DocumentKind m_kind;
    flow::Document m_model;
    std::unique_ptr<QTextDocument> m_doc;
    QList<flow::Heading> m_headings;
    QRectF m_body;  // body area on the page
    int m_pageCount = 1;
    bool m_hasFields = false;
    QHash<int, std::shared_ptr<QTextDocument>> m_hfCache;  // key: page*2 + isFooter
};
