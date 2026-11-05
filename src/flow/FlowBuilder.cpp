// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "FlowBuilder.h"

#include "core/FontSubstitution.h"
#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QUrl>

#include <cmath>

namespace flow {

namespace {

constexpr double kListIndentStep = 18.0;
using SafeImage::clampLength;
constexpr int kMaxNesting = 16;

QTextListFormat::Style listStyle(const ListLevel &l, bool *approximated)
{
    switch (l.marker) {
    case ListMarker::None: return QTextListFormat::ListStyleUndefined;
    case ListMarker::Disc: return QTextListFormat::ListDisc;
    case ListMarker::Circle: return QTextListFormat::ListCircle;
    case ListMarker::Square: return QTextListFormat::ListSquare;
    case ListMarker::Decimal: return QTextListFormat::ListDecimal;
    case ListMarker::LowerAlpha: return QTextListFormat::ListLowerAlpha;
    case ListMarker::UpperAlpha: return QTextListFormat::ListUpperAlpha;
    case ListMarker::LowerRoman: return QTextListFormat::ListLowerRoman;
    case ListMarker::UpperRoman: return QTextListFormat::ListUpperRoman;
    case ListMarker::Text: {
        // Qt draws only built-in bullet shapes; map common glyphs.
        const QString b = l.bulletText.trimmed();
        if (b.isEmpty() || b == u"•" || b == u"●") return QTextListFormat::ListDisc;
        if (b == u"◦" || b == u"o" || b == u"○") return QTextListFormat::ListCircle;
        if (b == u"▪" || b == u"■" || b == u"◾" || b == u"§")
            return QTextListFormat::ListSquare;
        *approximated = true;
        return QTextListFormat::ListDisc;
    }
    }
    return QTextListFormat::ListDisc;
}

}  // namespace

Builder::Builder(const Document &model, FidelityReport *report)
    : m_model(model), m_report(report)
{
}

QPaintDevice *Builder::pointDevice()
{
    // 72 dpi raster: font point sizes map 1:1 onto layout units.
    static QImage *device = [] {
        auto *img = new QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        img->setDotsPerMeterX(qRound(72.0 / 0.0254));
        img->setDotsPerMeterY(qRound(72.0 / 0.0254));
        return img;
    }();
    return device;
}

void Builder::prepare(QTextDocument *doc, const CharStyle &defaults)
{
    doc->setUndoRedoEnabled(false);
    doc->setDocumentMargin(0);
    doc->setUseDesignMetrics(true);
    doc->setIndentWidth(kListIndentStep);
    doc->documentLayout()->setPaintDevice(pointDevice());
    QFont f(defaults.family.isEmpty() ? QStringLiteral("Times New Roman") : defaults.family);
    f.setPointSizeF(defaults.size > 0 ? defaults.size : 12);
    doc->setDefaultFont(f);
    QTextOption opt = doc->defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc->setDefaultTextOption(opt);
}

void Builder::build(const Blocks &blocks, QTextDocument *doc, Context ctx)
{
    m_doc = doc;
    m_ctx = ctx;
    m_lists.clear();
    m_headings.clear();
    for (auto it = m_model.images.cbegin(); it != m_model.images.cend(); ++it)
        doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img:") + it.key()),
                         it.value());
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    bool atEmpty = true;
    insertBlocks(cursor, blocks, atEmpty);
    cursor.endEditBlock();
}

void Builder::insertBlocks(QTextCursor &cursor, const Blocks &blocks, bool &atEmptyBlock)
{
    for (const Block &b : blocks) {
        if (b.kind == Block::Para)
            insertParagraph(cursor, b.para, atEmptyBlock);
        else if (b.table)
            insertTable(cursor, *b.table, atEmptyBlock);
    }
}

QTextCharFormat Builder::charFormat(const CharStyle &s)
{
    QTextCharFormat cf;
    const QString family = s.family.isEmpty() ? m_model.defaultChar.family : s.family;
    if (!family.isEmpty())
        cf.setFontFamilies({FontSubstitution::resolve(family, m_report)});
    double size = s.size > 0 ? s.size : (m_model.defaultChar.size > 0 ? m_model.defaultChar.size : 12);
    if (!(size >= 1 && size <= Limits::kMaxFontSize)) size = qBound(1.0, std::isfinite(size) ? size : 12.0, Limits::kMaxFontSize);
    cf.setFontPointSize(size);
    cf.setFontWeight(s.bold ? QFont::Bold : QFont::Normal);
    cf.setFontItalic(s.italic);
    cf.setFontUnderline(s.underline);
    cf.setFontStrikeOut(s.strike);
    if (s.smallCaps) cf.setFontCapitalization(QFont::SmallCaps);
    else if (s.allCaps) cf.setFontCapitalization(QFont::AllUppercase);
    if (s.vertical > 0) cf.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (s.vertical < 0) cf.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    cf.setForeground(s.color.isValid() ? s.color : QColor(Qt::black));
    if (s.background.isValid() && s.background.alpha() > 0) cf.setBackground(s.background);
    if (s.letterSpacing != 0) {
        cf.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        cf.setFontLetterSpacing(qBound(-size, clampLength(s.letterSpacing), size * 4));
    }
    return cf;
}

QTextBlockFormat Builder::blockFormat(const ParaStyle &s, int listLevel)
{
    QTextBlockFormat bf;
    bf.setAlignment(s.align);
    // Every length comes from the file: keep it finite and page-sized.
    double left = clampLength(s.leftIndent);
    if (listLevel >= 0) left = qMax(0.0, left - (listLevel + 1) * kListIndentStep);
    bf.setLeftMargin(left);
    bf.setRightMargin(clampLength(s.rightIndent));
    bf.setTopMargin(qMax(0.0, clampLength(s.spaceBefore)));
    bf.setBottomMargin(qMax(0.0, clampLength(s.spaceAfter)));
    bf.setTextIndent(listLevel >= 0 ? 0 : clampLength(s.firstLineIndent));
    switch (s.lineRule) {
    case ParaStyle::LineRule::Auto: break;
    case ParaStyle::LineRule::Proportional:
        bf.setLineHeight(qBound(50.0, s.lineValue, 400.0), QTextBlockFormat::ProportionalHeight);
        break;
    case ParaStyle::LineRule::Exact:
        bf.setLineHeight(qBound(1.0, clampLength(s.lineValue), 2000.0), QTextBlockFormat::FixedHeight);
        break;
    case ParaStyle::LineRule::AtLeast:
        bf.setLineHeight(qBound(1.0, clampLength(s.lineValue), 2000.0), QTextBlockFormat::MinimumHeight);
        break;
    }
    QTextFormat::PageBreakFlags flags = QTextFormat::PageBreak_Auto;
    if (s.pageBreakBefore) flags |= QTextFormat::PageBreak_AlwaysBefore;
    if (s.pageBreakAfter) flags |= QTextFormat::PageBreak_AlwaysAfter;
    bf.setPageBreakPolicy(flags);
    if (!s.tabs.isEmpty()) {
        QList<QTextOption::Tab> tabs;
        for (const TabStop &t : s.tabs.mid(0, Limits::kMaxTabStops)) {
            QTextOption::TabType type = QTextOption::LeftTab;
            if (t.type == TabStop::Right) type = QTextOption::RightTab;
            else if (t.type == TabStop::Center) type = QTextOption::CenterTab;
            else if (t.type == TabStop::Decimal) type = QTextOption::DelimiterTab;
            tabs << QTextOption::Tab(qMax(0.0, clampLength(t.position)), type, t.type == TabStop::Decimal ? QChar('.') : QChar());
        }
        bf.setTabPositions(tabs);
    }
    if (s.headingLevel > 0) bf.setHeadingLevel(s.headingLevel);
    if (s.background.isValid() && s.background.alpha() > 0) bf.setBackground(s.background);
    return bf;
}

QTextList *Builder::listFor(QTextCursor &cursor, const ListRef &ref)
{
    const quint64 key = (quint64(quint32(ref.listId)) << 8) | quint64(ref.level & 0xff);
    if (QTextList *l = m_lists.value(key)) {
        l->add(cursor.block());
        return l;
    }
    bool approximated = false;
    QTextListFormat lf;
    lf.setStyle(listStyle(ref.format, &approximated));
    if (approximated && m_report) m_report->note(QStringLiteral("Some bullet glyphs approximated"));
    lf.setIndent(ref.level + 1);
    lf.setNumberPrefix(ref.format.prefix);
    lf.setNumberSuffix(ref.format.suffix);
    lf.setStart(ref.format.start);
    QTextList *l = cursor.createList(lf);
    m_lists.insert(key, l);
    return l;
}

void Builder::insertParagraph(QTextCursor &cursor, const Paragraph &p, bool &atEmptyBlock)
{
    const int listLevel = p.list && p.list->format.marker != ListMarker::None ? p.list->level : -1;
    const QTextBlockFormat bf = blockFormat(p.style, listLevel);
    const QTextCharFormat markFormat = charFormat(p.markStyle);
    if (atEmptyBlock) {
        cursor.setBlockFormat(bf);
        // Merge, never replace: in a table cell this format also holds the
        // cell's span and borders.
        cursor.mergeBlockCharFormat(markFormat);
        atEmptyBlock = false;
    } else {
        cursor.insertBlock(bf, markFormat);
    }
    if (listLevel >= 0) {
        listFor(cursor, *p.list);
        // An item at this level restarts any deeper sub-lists, like word
        // processors do.
        const quint32 id = quint32(p.list->listId);
        for (auto it = m_lists.begin(); it != m_lists.end();) {
            if ((it.key() >> 8) == id && int(it.key() & 0xff) > p.list->level)
                it = m_lists.erase(it);
            else
                ++it;
        }
    }
    if (p.style.headingLevel > 0) {
        const QString text = p.plainText().simplified();
        if (!text.isEmpty())
            m_headings << Heading{p.style.headingLevel, text, cursor.block().position()};
    }

    for (const Run &r : p.runs) {
        if (r.style.hidden) continue;
        QTextCharFormat cf = charFormat(r.style);
        if (!r.href.isEmpty()) {
            cf.setAnchor(true);
            cf.setAnchorHref(r.href);
        }
        if (!r.imageKey.isEmpty()) {
            QTextImageFormat img;
            img.setName(QStringLiteral("img:") + r.imageKey);
            if (r.imageSize.isValid()) {
                img.setWidth(qBound(1.0, clampLength(r.imageSize.width()), 5000.0));
                img.setHeight(qBound(1.0, clampLength(r.imageSize.height()), 5000.0));
            }
            cursor.insertImage(img);
            continue;
        }
        QString text;
        switch (r.field) {
        case Field::PageNumber: text = QString::number(m_ctx.pageNumber); break;
        case Field::PageCount: text = QString::number(m_ctx.pageCount); break;
        case Field::None: text = r.text; break;
        }
        if (text.isEmpty()) continue;
        text.replace(QLatin1Char('\n'), QChar::LineSeparator);
        text.replace(QLatin1Char('\r'), QChar::LineSeparator);
        cursor.insertText(text, cf);
    }
}

void Builder::insertTable(QTextCursor &cursor, const Table &t, bool &atEmptyBlock)
{
    const int rows = t.rows.size();
    const int cols = qMin(t.columnCount(), 256);
    if (rows == 0 || cols == 0) return;
    // Importers enforce a per-document cell budget; this guards any other
    // producer of FlowModel tables.
    if (qint64(rows) * cols > Limits::kMaxTableCells) {
        if (m_report) m_report->count(QStringLiteral("Tables too large to show"));
        return;
    }
    if (m_nesting >= kMaxNesting) {
        if (m_report) m_report->count(QStringLiteral("Deeply nested tables skipped"));
        return;
    }
    ++m_nesting;

    QTextTableFormat tf;
    tf.setBorder(0);
    tf.setBorderCollapse(true);
    tf.setCellSpacing(0);
    tf.setCellPadding(0);
    tf.setHeaderRowCount(qMin(t.headerRows, rows));
    tf.setAlignment(t.align);
    tf.setLeftMargin(qMax(0.0, clampLength(t.leftIndent)));
    if (t.width > 0) tf.setWidth(QTextLength(QTextLength::FixedLength, clampLength(t.width)));
    else tf.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    if (t.columnWidths.size() >= cols) {
        double total = 0;
        for (int c = 0; c < cols; ++c) total += qMax(0.0, clampLength(t.columnWidths[c]));
        QList<QTextLength> widths;
        for (int c = 0; c < cols; ++c) {
            if (t.width > 0 || total <= 0)
                widths << QTextLength(QTextLength::FixedLength, qMax(0.0, clampLength(t.columnWidths[c])));
            else
                widths << QTextLength(QTextLength::PercentageLength,
                                      100.0 * qMax(0.0, clampLength(t.columnWidths[c])) / total);
        }
        tf.setColumnWidthConstraints(widths);
    }

    if (!atEmptyBlock) cursor.insertBlock();
    atEmptyBlock = false;
    QTextTable *table = cursor.insertTable(rows, cols, tf);

    // Merge spans first; cell positions are grid positions.
    for (int r = 0; r < rows; ++r) {
        const Row &row = t.rows[r];
        for (int c = 0; c < row.cells.size() && c < cols; ++c) {
            const Cell &cell = row.cells[c];
            if (cell.covered || (cell.rowSpan <= 1 && cell.colSpan <= 1)) continue;
            table->mergeCells(r, c, qMin(cell.rowSpan, rows - r), qMin(cell.colSpan, cols - c));
        }
    }
    for (int r = 0; r < rows; ++r) {
        const Row &row = t.rows[r];
        for (int c = 0; c < row.cells.size() && c < cols; ++c) {
            const Cell &cell = row.cells[c];
            if (cell.covered) continue;
            QTextTableCell tc = table->cellAt(r, c);
            if (!tc.isValid() || tc.row() != r || tc.column() != c) continue;
            QTextTableCellFormat cf;
            if (cell.background.isValid() && cell.background.alpha() > 0) cf.setBackground(cell.background);
            auto side = [](const Border &b, auto setW, auto setBrush, auto setStyle) {
                setW(qBound(0.0, clampLength(b.width), 12.0));
                setBrush(QBrush(b.color));
                setStyle(b.width > 0 ? QTextFrameFormat::BorderStyle_Solid
                                     : QTextFrameFormat::BorderStyle_None);
            };
            side(cell.top, [&](qreal w) { cf.setTopBorder(w); },
                 [&](QBrush b) { cf.setTopBorderBrush(b); },
                 [&](QTextFrameFormat::BorderStyle s) { cf.setTopBorderStyle(s); });
            side(cell.bottom, [&](qreal w) { cf.setBottomBorder(w); },
                 [&](QBrush b) { cf.setBottomBorderBrush(b); },
                 [&](QTextFrameFormat::BorderStyle s) { cf.setBottomBorderStyle(s); });
            side(cell.left, [&](qreal w) { cf.setLeftBorder(w); },
                 [&](QBrush b) { cf.setLeftBorderBrush(b); },
                 [&](QTextFrameFormat::BorderStyle s) { cf.setLeftBorderStyle(s); });
            side(cell.right, [&](qreal w) { cf.setRightBorder(w); },
                 [&](QBrush b) { cf.setRightBorderBrush(b); },
                 [&](QTextFrameFormat::BorderStyle s) { cf.setRightBorderStyle(s); });
            cf.setPadding(qBound(0.0, clampLength(cell.padding), 72.0));
            if (cell.vAlign & Qt::AlignVCenter) cf.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            else if (cell.vAlign & Qt::AlignBottom) cf.setVerticalAlignment(QTextCharFormat::AlignBottom);
            QTextCursor cc = tc.firstCursorPosition();
            bool empty = true;
            insertBlocks(cc, cell.blocks, empty);
            // The cell format shares storage with its first block's char
            // format, so apply it after the content, merged.
            QTextCharFormat merged = tc.format();
            merged.merge(cf);
            tc.setFormat(merged);
        }
    }
    cursor = table->lastCursorPosition();
    cursor.movePosition(QTextCursor::NextBlock);
    atEmptyBlock = true;
    --m_nesting;
}

}  // namespace flow
