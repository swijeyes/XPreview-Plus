// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/FidelityReport.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMarginsF>
#include <QMap>
#include <QSizeF>
#include <QString>

#include <memory>
#include <optional>

// Format-neutral intermediate model shared by the ODT and RTF importers.
// All styles are already resolved; the builder knows nothing about ODF or RTF.
// Lengths are in points.
namespace flow {

struct CharStyle {
    QString family;
    double size = 0;  // 0 = document default
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool smallCaps = false;
    bool allCaps = false;
    int vertical = 0;  // 0 normal, 1 superscript, -1 subscript
    QColor color;
    QColor background;
    double letterSpacing = 0;
    bool hidden = false;
};

enum class Field { None, PageNumber, PageCount };

struct Run {
    QString text;
    CharStyle style;
    QString href;
    Field field = Field::None;
    // Inline image: key into Document::images, sized in points.
    QString imageKey;
    QSizeF imageSize;
};

enum class ListMarker {
    None, Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman, Text
};

struct ListLevel {
    ListMarker marker = ListMarker::Disc;
    QString bulletText;  // for Text markers (custom bullet glyph)
    QString prefix, suffix = QStringLiteral(".");
    int start = 1;
    double indent = 0;  // left indent of the text block for this level
};

struct ListRef {
    int listId = -1;  // paragraphs with equal id and level share a counter
    int level = 0;
    ListLevel format;
};

struct TabStop {
    double position = 0;
    enum Type { Left, Right, Center, Decimal } type = Left;
};

struct ParaStyle {
    Qt::Alignment align = Qt::AlignLeft;
    double leftIndent = 0, rightIndent = 0, firstLineIndent = 0;
    double spaceBefore = 0, spaceAfter = 0;
    // Line height: proportional (percent), exact or minimum, in points.
    enum class LineRule { Auto, Proportional, Exact, AtLeast } lineRule = LineRule::Auto;
    double lineValue = 100;
    bool pageBreakBefore = false;
    bool pageBreakAfter = false;
    bool keepWithNext = false;
    QList<TabStop> tabs;
    int headingLevel = 0;  // 0 = body text
    QColor background;
    QColor borderColor;  // paragraph border, if any
    double borderWidth = 0;
};

struct Block;
using Blocks = QList<Block>;

struct Paragraph {
    ParaStyle style;
    QList<Run> runs;
    std::optional<ListRef> list;
    CharStyle markStyle;  // character style for empty paragraphs / list marker
    QString plainText() const {
        QString s;
        for (const Run &r : runs) s += r.text;
        return s;
    }
};

struct Border {
    double width = 0;  // 0 = none
    QColor color = Qt::black;
};

struct Cell {
    Blocks blocks;
    int rowSpan = 1, colSpan = 1;
    bool covered = false;  // part of another cell's span
    QColor background;
    Border top, left, bottom, right;
    double padding = 2;
    Qt::Alignment vAlign = Qt::AlignTop;
};

struct Row {
    QList<Cell> cells;
    double height = 0;  // minimum height, 0 = auto
};

struct Table {
    QList<double> columnWidths;  // points; empty = equal split
    QList<Row> rows;
    int headerRows = 0;
    Qt::Alignment align = Qt::AlignLeft;
    double leftIndent = 0;
    double width = 0;  // 0 = full width
    // Rows hold the full grid (spanned positions are `covered` cells).
    int columnCount() const {
        qsizetype n = columnWidths.size();
        for (const Row &r : rows) n = qMax(n, r.cells.size());
        return int(n);
    }
};

struct Block {
    enum Kind { Para, TableBlock } kind = Para;
    Paragraph para;
    std::shared_ptr<Table> table;

    static Block paragraph(Paragraph p) {
        Block b;
        b.para = std::move(p);
        return b;
    }
    static Block tableBlock(Table t) {
        Block b;
        b.kind = TableBlock;
        b.table = std::make_shared<Table>(std::move(t));
        return b;
    }
};

struct PageSetup {
    QSizeF size{595.276, 841.89};      // A4
    QMarginsF margins{72, 72, 72, 72}; // body area
    double headerDistance = 36;  // top edge -> header top
    double footerDistance = 36;  // bottom edge -> footer bottom
    // ODF puts the header inside the page margins and pushes the body down.
    bool headerInsideMargins = false;
    double headerSpacing = 0, footerSpacing = 0;
    QColor background;
};

struct Document {
    PageSetup page;
    CharStyle defaultChar;  // family + size used when a run leaves them unset
    Blocks body;
    Blocks header, headerFirst;
    Blocks footer, footerFirst;
    bool titlePage = false;  // use *First variants on page 1
    Blocks notes;            // footnotes/endnotes collected at the end
    QHash<QString, QImage> images;
    QMap<QString, QString> metadata;
    FidelityReport report;
};

}  // namespace flow
