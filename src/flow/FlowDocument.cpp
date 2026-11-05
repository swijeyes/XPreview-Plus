// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "FlowDocument.h"

#include "core/SafeImage.h"

#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QThread>

#include <cmath>

namespace {

bool blocksHaveFields(const flow::Blocks &blocks)
{
    for (const flow::Block &b : blocks) {
        if (b.kind == flow::Block::Para) {
            for (const flow::Run &r : b.para.runs)
                if (r.field != flow::Field::None) return true;
        } else if (b.table) {
            for (const flow::Row &row : b.table->rows)
                for (const flow::Cell &c : row.cells)
                    if (blocksHaveFields(c.blocks)) return true;
        }
    }
    return false;
}

QAbstractTextDocumentLayout::PaintContext blackTextContext(const QRectF &clip)
{
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.clip = clip;
    // Pages are always white paper with black default text, even when the
    // UI is in dark mode.
    ctx.palette.setColor(QPalette::Text, Qt::black);
    ctx.palette.setColor(QPalette::Link, QColor(0, 0, 238));
    return ctx;
}

}  // namespace

FlowDocument::FlowDocument(DocumentKind kind, flow::Document model, const QString &path)
    : m_kind(kind), m_model(std::move(model))
{
    setFilePath(path);
    m_report = m_model.report;
    // Importers sanitise, but this class must be safe for any model.
    flow::PageSetup &setup = m_model.page;
    setup.size = SafeImage::sanitizePageSize(setup.size, QSizeF(595.276, 841.89));
    auto clampMargin = [](double v, double side) { return qBound(0.0, std::isfinite(v) ? v : 0.0, side / 2.5); };
    setup.margins = QMarginsF(clampMargin(setup.margins.left(), setup.size.width()),
                              clampMargin(setup.margins.top(), setup.size.height()),
                              clampMargin(setup.margins.right(), setup.size.width()),
                              clampMargin(setup.margins.bottom(), setup.size.height()));
    setup.headerDistance = clampMargin(setup.headerDistance, setup.size.height());
    setup.footerDistance = clampMargin(setup.footerDistance, setup.size.height());
    setup.headerSpacing = clampMargin(setup.headerSpacing, setup.size.height());
    setup.footerSpacing = clampMargin(setup.footerSpacing, setup.size.height());
    const flow::PageSetup &ps = m_model.page;
    QMarginsF m = ps.margins;
    // Keep at least 1 inch x 1 inch of body.
    if (ps.size.width() - m.left() - m.right() < 72) m.setLeft(18), m.setRight(18);
    if (ps.size.height() - m.top() - m.bottom() < 72) m.setTop(18), m.setBottom(18);

    m_hasFields = blocksHaveFields(m_model.header) || blocksHaveFields(m_model.footer) ||
                  blocksHaveFields(m_model.headerFirst) || blocksHaveFields(m_model.footerFirst);

    double top = m.top(), bottom = m.bottom();
    if (ps.headerInsideMargins) {
        // ODF: the header occupies the top of the margin box and pushes the
        // body down by its height plus spacing.
        const double hh = blocksHeight(m_model.header);
        if (hh > 0) top += hh + ps.headerSpacing;
        const double fh = blocksHeight(m_model.footer);
        if (fh > 0) bottom += fh + ps.footerSpacing;
    }
    m_body = QRectF(m.left(), top, ps.size.width() - m.left() - m.right(),
                    qMax(72.0, ps.size.height() - top - bottom));

    flow::Blocks body = m_model.body;
    if (!m_model.notes.isEmpty()) {
        flow::Paragraph heading;
        heading.style.spaceBefore = 18;
        heading.style.spaceAfter = 6;
        flow::Run r;
        r.text = QStringLiteral("Notes");
        r.style.bold = true;
        heading.runs << r;
        body << flow::Block::paragraph(heading) << m_model.notes;
        m_report.note(QStringLiteral("Footnotes are shown at the end of the document"));
    }

    m_doc = std::make_unique<QTextDocument>();
    flow::Builder::prepare(m_doc.get(), m_model.defaultChar);
    m_doc->setPageSize(m_body.size());
    flow::Builder builder(m_model, &m_report);
    builder.build(body, m_doc.get());
    m_headings = builder.headings();
    m_pageCount = qMax(1, m_doc->pageCount());
    // Model blocks are no longer needed once the document is built.
    m_model.body.clear();
    m_model.notes.clear();
}

FlowDocument::~FlowDocument() = default;

void FlowDocument::moveToThread(QThread *thread)
{
    std::lock_guard lock(mutex());
    m_doc->moveToThread(thread);
}

const flow::Blocks &FlowDocument::headerFor(int page) const
{
    return page == 0 && m_model.titlePage ? m_model.headerFirst : m_model.header;
}

const flow::Blocks &FlowDocument::footerFor(int page) const
{
    return page == 0 && m_model.titlePage ? m_model.footerFirst : m_model.footer;
}

std::unique_ptr<QTextDocument> FlowDocument::buildHeaderFooter(const flow::Blocks &blocks, int page)
{
    auto doc = std::make_unique<QTextDocument>();
    flow::Builder::prepare(doc.get(), m_model.defaultChar);
    const QMarginsF &m = m_model.page.margins;
    doc->setTextWidth(m_model.page.size.width() - m.left() - m.right());
    flow::Builder b(m_model, nullptr);
    b.build(blocks, doc.get(), {page + 1, m_pageCount});
    return doc;
}

double FlowDocument::blocksHeight(const flow::Blocks &blocks)
{
    if (blocks.isEmpty()) return 0;
    return buildHeaderFooter(blocks, 0)->size().height();
}

void FlowDocument::paintHeaderFooter(int page, QPainter *painter)
{
    const flow::PageSetup &ps = m_model.page;
    for (int isFooter = 0; isFooter < 2; ++isFooter) {
        const flow::Blocks &blocks = isFooter ? footerFor(page) : headerFor(page);
        if (blocks.isEmpty()) continue;
        const int key = (m_hasFields ? page : (page == 0 ? 0 : 1)) * 2 + isFooter;
        std::shared_ptr<QTextDocument> doc = m_hfCache.value(key);
        if (!doc) {
            doc = buildHeaderFooter(blocks, page);
            if (m_hfCache.size() > 64) m_hfCache.clear();
            m_hfCache.insert(key, doc);
        }
        const double h = doc->size().height();
        double y;
        if (ps.headerInsideMargins)
            y = isFooter ? ps.size.height() - ps.margins.bottom() - h : ps.margins.top();
        else
            y = isFooter ? ps.size.height() - ps.footerDistance - h : ps.headerDistance;
        painter->save();
        painter->translate(ps.margins.left(), y);
        doc->documentLayout()->draw(painter, blackTextContext(QRectF(QPointF(0, 0), doc->size())));
        painter->restore();
    }
}

void FlowDocument::paintPage(int page, QPainter *painter)
{
    std::lock_guard lock(mutex());
    if (m_model.page.background.isValid())
        painter->fillRect(QRectF(QPointF(0, 0), m_model.page.size), m_model.page.background);
    painter->save();
    painter->translate(m_body.topLeft());
    const double h = m_body.height();
    // Let content that overhangs the body a little (wide tables, images)
    // still show, but never bleed into neighbouring pages.
    painter->setClipRect(QRectF(-m_body.left(), 0, m_model.page.size.width(), h), Qt::IntersectClip);
    painter->translate(0, -page * h);
    m_doc->documentLayout()->draw(painter,
                                  blackTextContext(QRectF(-m_body.left(), page * h,
                                                          m_model.page.size.width(), h)));
    painter->restore();
    paintHeaderFooter(page, painter);
}

QVector<PageRect> FlowDocument::rangeBoxes(int from, int to)
{
    QVector<PageRect> out;
    auto *layout = m_doc->documentLayout();
    const double h = m_body.height();
    for (QTextBlock b = m_doc->findBlock(from); b.isValid() && b.position() <= to; b = b.next()) {
        QTextLayout *tl = b.layout();
        if (!tl) continue;
        const QRectF br = layout->blockBoundingRect(b);
        const int bpos = b.position();
        for (int i = 0; i < tl->lineCount(); ++i) {
            const QTextLine line = tl->lineAt(i);
            const int ls = bpos + line.textStart(), le = ls + line.textLength();
            const int s = qMax(from, ls), e = qMin(to, le);
            if (s >= e && !(s == e && from == to)) continue;
            const double x1 = line.cursorToX(s - bpos), x2 = line.cursorToX(e - bpos);
            QRectF r(br.x() + qMin(x1, x2), br.y() + line.y(), qAbs(x2 - x1), line.height());
            const int page = qBound(0, int(r.center().y() / h), m_pageCount - 1);
            r.translate(m_body.left(), m_body.top() - page * h);
            out << PageRect{page, r};
        }
    }
    return out;
}

QVector<TextHit> FlowDocument::search(const QString &query, bool caseSensitive)
{
    QVector<TextHit> hits;
    if (query.isEmpty()) return hits;
    std::lock_guard lock(mutex());
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor c(m_doc.get());
    while (hits.size() < 5000) {
        c = m_doc->find(query, c, flags);
        if (c.isNull()) break;
        TextHit hit;
        hit.text = c.selectedText();
        hit.boxes = rangeBoxes(c.selectionStart(), c.selectionEnd());
        if (!hit.boxes.isEmpty()) hits << hit;
    }
    return hits;
}

QPointF FlowDocument::toDocPoint(int page, QPointF pt) const
{
    return QPointF(pt.x() - m_body.left(), pt.y() - m_body.top() + page * m_body.height());
}

TextHit FlowDocument::selectText(int page, QPointF from, QPointF to)
{
    TextHit hit;
    std::lock_guard lock(mutex());
    auto *layout = m_doc->documentLayout();
    const double h = m_body.height();
    auto clampY = [&](QPointF p) {
        p.setY(qBound(page * h, p.y(), (page + 1) * h - 0.01));
        return p;
    };
    int a = layout->hitTest(clampY(toDocPoint(page, from)), Qt::FuzzyHit);
    int b = layout->hitTest(clampY(toDocPoint(page, to)), Qt::FuzzyHit);
    if (a < 0 || b < 0) return hit;
    if (a > b) std::swap(a, b);
    if (a == b) return hit;
    QTextCursor c(m_doc.get());
    c.setPosition(a);
    c.setPosition(b, QTextCursor::KeepAnchor);
    hit.text = c.selectedText();
    hit.text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    hit.text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    hit.boxes = rangeBoxes(a, b);
    return hit;
}

QString FlowDocument::pageText(int page)
{
    std::lock_guard lock(mutex());
    auto *layout = m_doc->documentLayout();
    const double h = m_body.height();
    const int a = layout->hitTest(QPointF(0, page * h), Qt::FuzzyHit);
    const int b = layout->hitTest(QPointF(m_body.width(), (page + 1) * h - 0.01), Qt::FuzzyHit);
    if (a < 0 || b <= a) return {};
    QTextCursor c(m_doc.get());
    c.setPosition(a);
    c.setPosition(b, QTextCursor::KeepAnchor);
    QString t = c.selectedText();
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return t;
}

QString FlowDocument::linkAt(int page, QPointF pt)
{
    std::lock_guard lock(mutex());
    return m_doc->documentLayout()->anchorAt(toDocPoint(page, pt));
}

QList<OutlineItem> FlowDocument::outline()
{
    std::lock_guard lock(mutex());
    // Nest headings by level.
    QList<OutlineItem> roots;
    QList<std::pair<int, QList<OutlineItem> *>> stack;  // level, container
    const double h = m_body.height();
    for (const flow::Heading &hd : std::as_const(m_headings)) {
        QTextBlock b = m_doc->findBlock(hd.position);
        const QRectF r = m_doc->documentLayout()->blockBoundingRect(b);
        OutlineItem item;
        item.title = hd.text;
        item.page = qBound(0, int(r.top() / h), m_pageCount - 1);
        item.y = r.top() - item.page * h + m_body.top();
        while (!stack.isEmpty() && stack.last().first >= hd.level) stack.removeLast();
        QList<OutlineItem> *container = stack.isEmpty() ? &roots : stack.last().second;
        container->append(item);
        stack.append({hd.level, &container->last().children});
    }
    return roots;
}
