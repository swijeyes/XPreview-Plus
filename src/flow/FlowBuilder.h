// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "FlowModel.h"

#include <QTextCursor>
#include <QTextDocument>

class QPaintDevice;
class QTextList;

namespace flow {

struct Heading {
    int level;
    QString text;
    int position;  // document position of the heading block
};

// Values substituted into page-number fields.
struct PageContext {
    int pageNumber = 1;
    int pageCount = 1;
};

// Builds a QTextDocument from a FlowModel block list. Lengths in the model
// are points; the document is laid out on a 72 dpi device so one layout
// unit is one point on every OS.

class Builder {
public:
    using Context = PageContext;

    Builder(const Document &model, FidelityReport *report);

    void build(const Blocks &blocks, QTextDocument *doc, Context ctx = {});
    const QList<Heading> &headings() const { return m_headings; }

    static QPaintDevice *pointDevice();
    static void prepare(QTextDocument *doc, const CharStyle &defaults);

private:
    void insertBlocks(QTextCursor &cursor, const Blocks &blocks, bool &atEmptyBlock);
    void insertParagraph(QTextCursor &cursor, const Paragraph &p, bool &atEmptyBlock);
    void insertTable(QTextCursor &cursor, const Table &t, bool &atEmptyBlock);
    QTextCharFormat charFormat(const CharStyle &s);
    QTextBlockFormat blockFormat(const ParaStyle &s, int listLevel);
    QTextList *listFor(QTextCursor &cursor, const ListRef &ref);

    const Document &m_model;
    FidelityReport *m_report;
    QTextDocument *m_doc = nullptr;
    Context m_ctx;
    QHash<quint64, QTextList *> m_lists;
    QList<Heading> m_headings;
    int m_nesting = 0;
};

}  // namespace flow
