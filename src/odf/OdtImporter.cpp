// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "OdtImporter.h"

#include "OdfPackage.h"
#include "OdfStyles.h"
#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QFile>
#include <QXmlStreamReader>

#include <functional>

namespace odf {

namespace {

using namespace flow;

struct MasterPage {
    QString name, layout;
    Blocks header, footer, headerFirst, footerFirst;
    bool hasFirst = false;
};

// Inline parsing state for one paragraph.
struct InlineCtx {
    Paragraph *para;
    Blocks *extras;       // text boxes etc. flowed after the paragraph
    Props text;           // current character properties
    QString href;
    bool lastWasSpace = true;
};

class Importer {
public:
    Importer(OdfPackage *pkg, Document &doc) : m_pkg(pkg), m_doc(doc) {}

    // Parses one XML stream (styles.xml, content.xml, meta.xml or a flat
    // document); content from all of them accumulates.
    bool parse(const QByteArray &xml, QString *error);
    void finish();

private:
    void parseRoot(QXmlStreamReader &r);
    void parseMasterStyles(QXmlStreamReader &r);
    void parseMeta(QXmlStreamReader &r);
    void parseBlocks(QXmlStreamReader &r, Blocks &out, int depth);
    void parseParagraph(QXmlStreamReader &r, Blocks &out, int depth, int outlineLevel,
                        const std::optional<ListRef> &list, double listIndent);
    void parseInline(QXmlStreamReader &r, InlineCtx &ctx, int depth);
    void parseList(QXmlStreamReader &r, Blocks &out, int depth, int level, QString styleName, int listId);
    void parseTable(QXmlStreamReader &r, Blocks &out, int depth);
    void parseFrame(QXmlStreamReader &r, InlineCtx &ctx, int depth);
    void parseNote(QXmlStreamReader &r, InlineCtx &ctx, int depth);
    void appendText(InlineCtx &ctx, QStringView s);
    void appendRun(InlineCtx &ctx, Run run);
    QString loadImage(const QString &href, const QByteArray &inlineData);
    void skip(QXmlStreamReader &r, const QString &what);
    Border parseBorder(const QString &v);
    QString outlineNumber(int level);

    OdfPackage *m_pkg;
    Document &m_doc;
    Styles m_styles;
    QList<MasterPage> m_masters;
    QString m_firstMaster;  // master page named by the first paragraph
    bool m_sawBodyParagraph = false;
    int m_nextListId = 1;
    QHash<QString, int> m_listIdsByXmlId;
    QHash<QString, int> m_lastListIdForStyle;
    QList<int> m_outlineCounters = QList<int>(10, 0);
    int m_imageCounter = 0;
    QHash<QString, QString> m_imageKeys;
    int m_rowsBudget = Limits::kMaxTableRows;
    int m_cellBudget = Limits::kMaxTableCells;
    SafeImage::Budget m_imageBudget{Limits::kMaxDocumentImageBytes};
};

bool isWhite(QChar c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; }

bool sameStyle(const CharStyle &a, const CharStyle &b)
{
    return a.family == b.family && a.size == b.size && a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.strike == b.strike && a.smallCaps == b.smallCaps &&
           a.allCaps == b.allCaps && a.vertical == b.vertical && a.color == b.color &&
           a.background == b.background && a.letterSpacing == b.letterSpacing && a.hidden == b.hidden;
}

bool Importer::parse(const QByteArray &xml, QString *error)
{
    QXmlStreamReader r(xml);
    r.setNamespaceProcessing(true);
    while (!r.atEnd()) {
        if (r.readNext() == QXmlStreamReader::StartElement) {
            parseRoot(r);
        }
    }
    if (r.hasError()) {
        if (error) *error = QStringLiteral("XML error at line %1: %2").arg(r.lineNumber()).arg(r.errorString());
        return false;
    }
    return true;
}

void Importer::parseRoot(QXmlStreamReader &r)
{
    // r is at office:document / document-content / document-styles /
    // document-meta.
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (r.namespaceUri() != ns::office) {
            r.skipCurrentElement();
            continue;
        }
        if (n == u"font-face-decls") m_styles.readFontFaces(r);
        else if (n == u"styles" || n == u"automatic-styles") m_styles.readStyles(r);
        else if (n == u"master-styles") parseMasterStyles(r);
        else if (n == u"meta") parseMeta(r);
        else if (n == u"body") {
            while (r.readNextStartElement()) {
                if (r.name() == u"text") parseBlocks(r, m_doc.body, 0);
                else {
                    m_doc.report.error(QStringLiteral("Not a text document"));
                    r.skipCurrentElement();
                }
            }
        } else if (n == u"scripts" || n == u"forms") {
            r.skipCurrentElement();  // never run macros
        } else {
            r.skipCurrentElement();
        }
    }
}

void Importer::parseMeta(QXmlStreamReader &r)
{
    while (r.readNextStartElement()) {
        const QString ns = r.namespaceUri().toString();
        const QString n = r.name().toString();
        QString key;
        if (ns == ns::dc && n == u"title") key = QStringLiteral("Title");
        else if (ns == ns::dc && n == u"creator") key = QStringLiteral("Author");
        else if (ns == ns::meta && n == u"initial-creator") key = QStringLiteral("Created by");
        else if (ns == ns::dc && n == u"subject") key = QStringLiteral("Subject");
        else if (ns == ns::dc && n == u"date") key = QStringLiteral("Modified");
        else if (ns == ns::meta && n == u"creation-date") key = QStringLiteral("Created");
        else if (ns == ns::meta && n == u"generator") key = QStringLiteral("Generator");
        if (key.isEmpty()) {
            r.skipCurrentElement();
            continue;
        }
        const QString v = r.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        if (!v.isEmpty()) m_doc.metadata.insert(key, v.left(500));
    }
}

void Importer::parseMasterStyles(QXmlStreamReader &r)
{
    while (r.readNextStartElement()) {
        if (r.name() != u"master-page") {
            r.skipCurrentElement();
            continue;
        }
        MasterPage mp;
        mp.name = r.attributes().value(ns::style, u"name").toString();
        mp.layout = r.attributes().value(ns::style, u"page-layout-name").toString();
        while (r.readNextStartElement()) {
            const auto n = r.name();
            const bool display = r.attributes().value(ns::style, u"display") != u"false";
            Blocks *target = nullptr;
            if (n == u"header") target = &mp.header;
            else if (n == u"footer") target = &mp.footer;
            else if (n == u"header-first") { target = &mp.headerFirst; mp.hasFirst = true; }
            else if (n == u"footer-first") { target = &mp.footerFirst; mp.hasFirst = true; }
            if (target && display) parseBlocks(r, *target, 1);
            else r.skipCurrentElement();
        }
        m_masters << mp;
    }
}

void Importer::skip(QXmlStreamReader &r, const QString &what)
{
    if (!what.isEmpty()) m_doc.report.count(what);
    r.skipCurrentElement();
}

void Importer::parseBlocks(QXmlStreamReader &r, Blocks &out, int depth)
{
    if (depth > Limits::kMaxXmlDepth) {
        skip(r, QStringLiteral("Deeply nested content skipped"));
        return;
    }
    while (r.readNextStartElement()) {
        const auto n = r.name();
        const auto uri = r.namespaceUri();
        if (uri == ns::text && (n == u"p" || n == u"h")) {
            int level = 0;
            if (n == u"h") level = qBound(1, r.attributes().value(ns::text, u"outline-level").toInt(), 10);
            parseParagraph(r, out, depth + 1, level, std::nullopt, 0);
        } else if (uri == ns::text && n == u"list") {
            const QString style = r.attributes().value(ns::text, u"style-name").toString();
            parseList(r, out, depth + 1, 0, style, -1);
        } else if (uri == ns::table && n == u"table") {
            parseTable(r, out, depth + 1);
        } else if (uri == ns::text &&
                   (n == u"section" || n == u"index-body" || n == u"table-of-content" ||
                    n == u"alphabetical-index" || n == u"illustration-index" || n == u"table-index" ||
                    n == u"object-index" || n == u"user-index" || n == u"bibliography")) {
            if (n == u"section") {
                const Style s = m_styles.resolve(QStringLiteral("section"),
                                                 r.attributes().value(ns::text, u"style-name").toString());
                if (s.section.value(QStringLiteral("fo:column-count")).toInt() > 1)
                    m_doc.report.note(QStringLiteral("Multiple columns shown as one column"));
            }
            parseBlocks(r, out, depth + 1);
        } else if (uri == ns::draw && (n == u"frame" || n == u"a")) {
            // Frame anchored to the page or paragraph at block level.
            Paragraph p;
            Blocks extras;
            InlineCtx ctx{&p, &extras, {}, {}, true};
            if (n == u"a") parseInline(r, ctx, depth + 1);
            else parseFrame(r, ctx, depth + 1);
            if (!p.runs.isEmpty()) out << Block::paragraph(p);
            out << extras;
        } else if (n == u"custom-shape" || n == u"rect" || n == u"ellipse" || n == u"line" ||
                   n == u"polygon" || n == u"polyline" || n == u"path" || n == u"g" ||
                   n == u"connector" || n == u"circle") {
            skip(r, QStringLiteral("Drawing shapes not shown"));
        } else {
            // tracked-changes, sequence-decls, soft-page-break, forms, ...
            r.skipCurrentElement();
        }
    }
}

QString Importer::outlineNumber(int level)
{
    const QList<OutlineLevel> &o = m_styles.outline();
    if (level < 1 || level > o.size() || level > m_outlineCounters.size()) return {};
    const OutlineLevel &ol = o[level - 1];
    if (ol.format.isEmpty()) return {};
    if (m_outlineCounters[level - 1] == 0) m_outlineCounters[level - 1] = ol.start - 1;
    ++m_outlineCounters[level - 1];
    for (int i = level; i < m_outlineCounters.size(); ++i) m_outlineCounters[i] = 0;
    QStringList parts;
    for (int l = qMax(1, level - ol.displayLevels + 1); l <= level; ++l) {
        const int v = qMax(1, m_outlineCounters[l - 1]);
        const QString fmt = l - 1 < o.size() ? o[l - 1].format : QStringLiteral("1");
        if (fmt == u"a" || fmt == u"A") {
            QString s(QChar((fmt == u"a" ? 'a' : 'A') + (v - 1) % 26));
            parts << s;
        } else if (fmt == u"i" || fmt == u"I") {
            static const char *romans[] = {"i","ii","iii","iv","v","vi","vii","viii","ix","x"};
            QString s = QString::fromLatin1(v <= 10 ? romans[v - 1] : "x");
            parts << (fmt == u"I" ? s.toUpper() : s);
        } else {
            parts << QString::number(v);
        }
    }
    return ol.prefix + parts.join(QLatin1Char('.')) + ol.suffix;
}

void Importer::parseParagraph(QXmlStreamReader &r, Blocks &out, int depth, int outlineLevel,
                              const std::optional<ListRef> &list, double listIndent)
{
    const QString styleName = r.attributes().value(ns::text, u"style-name").toString();
    const ResolvedPara rp = m_styles.resolveParagraph(styleName);
    if (!m_sawBodyParagraph && &out == &m_doc.body) {
        m_sawBodyParagraph = true;
        m_firstMaster = rp.masterPage;
    } else if (&out == &m_doc.body && !rp.masterPage.isEmpty()) {
        // A new master page mid-document starts a new page.
    }
    Paragraph p;
    p.style = rp.para;
    if (!rp.masterPage.isEmpty() && &out == &m_doc.body && !out.isEmpty()) p.style.pageBreakBefore = true;
    if (outlineLevel == 0 && rp.outlineLevel > 0 && r.name() == u"h") outlineLevel = rp.outlineLevel;
    p.style.headingLevel = outlineLevel;
    p.markStyle = m_styles.toCharStyle(rp.textProps);
    if (list) {
        p.list = list;
        if (p.style.leftIndent <= 0) p.style.leftIndent = listIndent > 0 ? listIndent : (list->level + 1) * 18.0;
    }
    Blocks extras;
    InlineCtx ctx{&p, &extras, rp.textProps, {}, true};
    if (outlineLevel > 0) {
        const QString num = outlineNumber(outlineLevel);
        if (!num.isEmpty()) appendText(ctx, QString(num + QLatin1Char(' ')));
        ctx.lastWasSpace = true;
    }
    parseInline(r, ctx, depth + 1);
    // ODF ignores trailing white space in a paragraph.
    while (!p.runs.isEmpty() && p.runs.last().imageKey.isEmpty() && p.runs.last().field == Field::None) {
        QString &t = p.runs.last().text;
        while (!t.isEmpty() && t.back() == u' ') t.chop(1);
        if (!t.isEmpty()) break;
        p.runs.removeLast();
    }
    out << Block::paragraph(std::move(p));
    out << extras;
}

void Importer::appendRun(InlineCtx &ctx, Run run)
{
    QList<Run> &runs = ctx.para->runs;
    if (!runs.isEmpty() && run.imageKey.isEmpty() && run.field == Field::None) {
        Run &last = runs.last();
        if (last.imageKey.isEmpty() && last.field == Field::None && last.href == run.href &&
            sameStyle(last.style, run.style)) {
            last.text += run.text;
            return;
        }
    }
    runs << std::move(run);
}

void Importer::appendText(InlineCtx &ctx, QStringView s)
{
    // Collapse white space per ODF 1.3 section 6.1.2.
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        if (isWhite(c)) {
            if (ctx.lastWasSpace) continue;
            out += QLatin1Char(' ');
            ctx.lastWasSpace = true;
        } else {
            out += c;
            ctx.lastWasSpace = false;
        }
    }
    if (out.isEmpty()) return;
    Run run;
    run.text = out;
    run.style = m_styles.toCharStyle(ctx.text);
    run.href = ctx.href;
    appendRun(ctx, std::move(run));
}

void Importer::parseInline(QXmlStreamReader &r, InlineCtx &ctx, int depth)
{
    if (depth > Limits::kMaxXmlDepth) {
        skip(r, QStringLiteral("Deeply nested content skipped"));
        return;
    }
    while (!r.atEnd()) {
        const auto tok = r.readNext();
        if (tok == QXmlStreamReader::EndElement) return;
        if (tok == QXmlStreamReader::Characters) {
            appendText(ctx, r.text());
            continue;
        }
        if (tok != QXmlStreamReader::StartElement) continue;
        const auto n = r.name();
        const auto uri = r.namespaceUri();
        if (uri == ns::text) {
            if (n == u"span") {
                const Props saved = ctx.text;
                ctx.text = m_styles.resolveText(r.attributes().value(ns::text, u"style-name").toString(), ctx.text);
                parseInline(r, ctx, depth + 1);
                ctx.text = saved;
            } else if (n == u"a") {
                const QString saved = ctx.href;
                ctx.href = r.attributes().value(ns::xlink, u"href").toString();
                const Props savedText = ctx.text;
                const QString st = r.attributes().value(ns::text, u"style-name").toString();
                if (!st.isEmpty()) ctx.text = m_styles.resolveText(st, ctx.text);
                parseInline(r, ctx, depth + 1);
                ctx.href = saved;
                ctx.text = savedText;
            } else if (n == u"s") {
                const int c = qBound(1, r.attributes().hasAttribute(ns::text, u"c")
                                            ? r.attributes().value(ns::text, u"c").toInt() : 1, 1000);
                Run run;
                run.text = QString(c, QLatin1Char(' '));
                run.style = m_styles.toCharStyle(ctx.text);
                run.href = ctx.href;
                appendRun(ctx, std::move(run));
                ctx.lastWasSpace = false;
                r.skipCurrentElement();
            } else if (n == u"tab" || n == u"line-break") {
                Run run;
                run.text = n == u"tab" ? QStringLiteral("\t") : QString(QChar::LineSeparator);
                run.style = m_styles.toCharStyle(ctx.text);
                run.href = ctx.href;
                appendRun(ctx, std::move(run));
                ctx.lastWasSpace = n == u"line-break";
                r.skipCurrentElement();
            } else if (n == u"page-number" || n == u"page-count") {
                Run run;
                run.field = n == u"page-number" ? Field::PageNumber : Field::PageCount;
                run.style = m_styles.toCharStyle(ctx.text);
                appendRun(ctx, std::move(run));
                ctx.lastWasSpace = false;
                r.skipCurrentElement();
            } else if (n == u"note") {
                parseNote(r, ctx, depth + 1);
            } else if (n == u"ruby") {
                while (r.readNextStartElement()) {
                    if (r.name() == u"ruby-base") parseInline(r, ctx, depth + 1);
                    else r.skipCurrentElement();
                }
            } else if (n == u"tracked-changes" || n == u"bookmark" || n == u"bookmark-start" ||
                       n == u"bookmark-end" || n == u"reference-mark" || n == u"reference-mark-start" ||
                       n == u"reference-mark-end" || n == u"soft-page-break" || n == u"change" ||
                       n == u"change-start" || n == u"change-end" || n == u"alphabetical-index-mark" ||
                       n == u"toc-mark" || n == u"user-index-mark") {
                r.skipCurrentElement();
            } else {
                // Fields (date, title, author, chapter, sequence, ...) carry
                // their last value as text: keep it.
                parseInline(r, ctx, depth + 1);
            }
        } else if (uri == ns::draw) {
            if (n == u"frame") parseFrame(r, ctx, depth + 1);
            else if (n == u"a") {
                const QString saved = ctx.href;
                ctx.href = r.attributes().value(ns::xlink, u"href").toString();
                parseInline(r, ctx, depth + 1);
                ctx.href = saved;
            } else skip(r, QStringLiteral("Drawing shapes not shown"));
        } else if (uri == ns::office && n == u"annotation") {
            skip(r, QStringLiteral("Comments hidden"));
        } else if (uri == ns::office && n == u"annotation-end") {
            r.skipCurrentElement();
        } else {
            r.skipCurrentElement();
        }
    }
}

void Importer::parseNote(QXmlStreamReader &r, InlineCtx &ctx, int depth)
{
    QString citation;
    Blocks body;
    while (r.readNextStartElement()) {
        if (r.name() == u"note-citation") {
            citation = r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        } else if (r.name() == u"note-body") {
            parseBlocks(r, body, depth + 1);
        } else {
            r.skipCurrentElement();
        }
    }
    if (citation.isEmpty()) citation = QString::number(m_doc.notes.size() + 1);
    Run mark;
    mark.text = citation;
    mark.style = m_styles.toCharStyle(ctx.text);
    mark.style.vertical = 1;
    appendRun(ctx, mark);
    ctx.lastWasSpace = false;
    if (!body.isEmpty() && body.first().kind == Block::Para) {
        Run num;
        num.text = citation + QStringLiteral(". ");
        num.style = body.first().para.markStyle;
        body.first().para.runs.prepend(num);
    }
    m_doc.notes << body;
}

QString Importer::loadImage(const QString &href, const QByteArray &inlineData)
{
    const QString cacheKey = href.isEmpty() ? QString() : href;
    if (!cacheKey.isEmpty() && m_imageKeys.contains(cacheKey)) return m_imageKeys.value(cacheKey);
    QByteArray data = inlineData;
    if (data.isEmpty()) {
        // Only package-internal references; never fetch external URLs.
        if (!m_pkg || href.contains(QLatin1String("://")) || href.startsWith(QLatin1Char('/')))
            return {};
        data = m_pkg->read(href);
    }
    if (data.isEmpty()) return {};
    bool refused = false;
    const QImage img = SafeImage::decodeEmbedded(data, m_imageBudget, &refused);
    if (refused) m_doc.report.count(QStringLiteral("Images too large to show"));
    if (img.isNull()) return {};
    const QString key = QStringLiteral("odf%1").arg(++m_imageCounter);
    m_doc.images.insert(key, img);
    if (!cacheKey.isEmpty()) m_imageKeys.insert(cacheKey, key);
    return key;
}

void Importer::parseFrame(QXmlStreamReader &r, InlineCtx &ctx, int depth)
{
    const auto a = r.attributes();
    const double w = toPoints(a.value(ns::svg, u"width"));
    const double h = toPoints(a.value(ns::svg, u"height"));
    QString imageKey;
    bool sawObject = false, sawTextBox = false;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == u"image" && imageKey.isEmpty()) {
            const QString href = r.attributes().value(ns::xlink, u"href").toString();
            QByteArray inlineData;
            if (href.isEmpty()) {
                while (r.readNextStartElement()) {
                    if (r.name() == u"binary-data")
                        inlineData = QByteArray::fromBase64(r.readElementText().toLatin1());
                    else
                        r.skipCurrentElement();
                }
            } else {
                r.skipCurrentElement();
            }
            imageKey = loadImage(href, inlineData);
            if (imageKey.isEmpty()) m_doc.report.count(QStringLiteral("Images in unsupported formats shown as placeholders"));
        } else if (n == u"text-box") {
            sawTextBox = true;
            parseBlocks(r, *ctx.extras, depth + 1);
            m_doc.report.count(QStringLiteral("Text boxes flowed inline"));
        } else if (n == u"object" || n == u"object-ole") {
            sawObject = true;
            r.skipCurrentElement();
        } else {
            r.skipCurrentElement();
        }
    }
    Run run;
    run.style = m_styles.toCharStyle(ctx.text);
    run.href = ctx.href;
    if (!imageKey.isEmpty()) {
        run.imageKey = imageKey;
        const QImage &img = m_doc.images[imageKey];
        QSizeF size(w, h);
        if (size.width() <= 0 || size.height() <= 0) size = QSizeF(img.width() * 0.75, img.height() * 0.75);
        run.imageSize = QSizeF(qBound(1.0, size.width(), 5000.0), qBound(1.0, size.height(), 5000.0));
        if (sawObject) m_doc.report.count(QStringLiteral("Charts/objects shown as images"));
        appendRun(ctx, std::move(run));
        ctx.lastWasSpace = false;
    } else if (sawObject || !sawTextBox) {
        run.text = sawObject ? QStringLiteral("[Embedded object]") : QStringLiteral("[Image]");
        run.style.color = QColor(120, 120, 120);
        run.style.italic = true;
        if (sawObject) m_doc.report.count(QStringLiteral("Embedded objects shown as placeholders"));
        appendRun(ctx, std::move(run));
        ctx.lastWasSpace = false;
    }
}

void Importer::parseList(QXmlStreamReader &r, Blocks &out, int depth, int level, QString styleName,
                         int listId)
{
    if (depth > Limits::kMaxXmlDepth) {
        skip(r, QStringLiteral("Deeply nested content skipped"));
        return;
    }
    const auto a = r.attributes();
    const QString own = a.value(ns::text, u"style-name").toString();
    if (!own.isEmpty()) styleName = own;
    if (listId < 0) {
        const QString cont = a.value(ns::text, u"continue-list").toString();
        const bool continueNumbering = a.value(ns::text, u"continue-numbering") == u"true";
        if (!cont.isEmpty() && m_listIdsByXmlId.contains(cont)) listId = m_listIdsByXmlId.value(cont);
        else if (continueNumbering && m_lastListIdForStyle.contains(styleName))
            listId = m_lastListIdForStyle.value(styleName);
        else listId = m_nextListId++;
        const QString xmlId = a.value(QStringLiteral("http://www.w3.org/XML/1998/namespace"), u"id").toString();
        if (!xmlId.isEmpty()) m_listIdsByXmlId.insert(xmlId, listId);
        m_lastListIdForStyle.insert(styleName, listId);
    }
    const ListStyle *ls = m_styles.listStyle(styleName);
    const int lvl = qMin(level, Limits::kMaxListLevels - 1);
    ListRef ref;
    ref.listId = listId;
    ref.level = lvl;
    ref.format = ls ? ls->level(lvl) : ListLevel{};
    if (!ls) {
        // Paragraph-style list (e.g. outline numbering) or missing style.
        ref.format.marker = ListMarker::Disc;
    }

    while (r.readNextStartElement()) {
        const bool header = r.name() == u"list-header";
        if (r.name() != u"list-item" && !header) {
            r.skipCurrentElement();
            continue;
        }
        bool first = true;
        while (r.readNextStartElement()) {
            const auto n = r.name();
            if (n == u"p" || n == u"h") {
                int outline = 0;
                if (n == u"h") outline = qBound(1, r.attributes().value(ns::text, u"outline-level").toInt(), 10);
                std::optional<ListRef> item;
                if (first && !header) item = ref;
                // Continuation paragraphs keep the indent but have no marker.
                parseParagraph(r, out, depth + 1, outline, item, ref.format.indent);
                if (!item && !out.isEmpty() && out.last().kind == Block::Para) {
                    Paragraph &p = out.last().para;
                    if (p.style.leftIndent <= 0)
                        p.style.leftIndent = ref.format.indent > 0 ? ref.format.indent : (lvl + 1) * 18.0;
                }
                first = false;
            } else if (n == u"list") {
                parseList(r, out, depth + 1, level + 1, styleName, listId);
                first = false;
            } else if (n == u"table" ) {
                parseTable(r, out, depth + 1);
            } else {
                r.skipCurrentElement();
            }
        }
    }
}

Border Importer::parseBorder(const QString &v)
{
    Border b;
    if (v.isEmpty() || v == u"none" || v == u"hidden") return b;
    const QStringList parts = v.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        bool ok = false;
        const double w = toPoints(p, &ok);
        if (ok && w > 0) b.width = qBound(0.25, w, 6.0);
        else if (p.startsWith(QLatin1Char('#'))) b.color = toColor(p);
        else if (p == u"thin") b.width = 0.5;
        else if (p == u"medium") b.width = 1.0;
        else if (p == u"thick") b.width = 2.0;
    }
    if (b.width == 0 && (v.contains(u"solid") || v.contains(u"double") || v.contains(u"dotted") || v.contains(u"dashed")))
        b.width = 0.5;
    return b;
}

void Importer::parseTable(QXmlStreamReader &r, Blocks &out, int depth)
{
    if (depth > Limits::kMaxXmlDepth) {
        skip(r, QStringLiteral("Deeply nested content skipped"));
        return;
    }
    Table t;
    const Style ts = m_styles.resolve(QStringLiteral("table"), r.attributes().value(ns::table, u"style-name").toString());
    bool ok = false;
    const double tw = toPoints(ts.table.value(QStringLiteral("style:width")), &ok);
    if (ok && tw > 0) t.width = tw;
    const QString align = ts.table.value(QStringLiteral("table:align"));
    if (align == u"center") t.align = Qt::AlignHCenter;
    else if (align == u"right") t.align = Qt::AlignRight;
    t.leftIndent = qMax(0.0, toPoints(ts.table.value(QStringLiteral("fo:margin-left"))));
    if (align == u"margins") t.width = 0;  // fill the text area

    QList<double> relWidths;
    bool allAbsolute = true;
    int headerRows = 0;

    std::function<void(int nesting, bool inHeader)> readContainer;
    auto readColumn = [&]() {
        const auto a = r.attributes();
        const int rep = qBound(1, a.hasAttribute(ns::table, u"number-columns-repeated")
                                       ? a.value(ns::table, u"number-columns-repeated").toInt() : 1,
                               Limits::kMaxTableColumns);
        const Style cs = m_styles.resolve(QStringLiteral("table-column"), a.value(ns::table, u"style-name").toString());
        bool aok = false;
        const double w = toPoints(cs.column.value(QStringLiteral("style:column-width")), &aok);
        const QString rel = cs.column.value(QStringLiteral("style:rel-column-width"));
        for (int i = 0; i < rep && t.columnWidths.size() < Limits::kMaxTableColumns; ++i) {
            t.columnWidths << (aok ? w : 0);
            relWidths << (rel.isEmpty() ? 1.0 : QStringView(rel).chopped(rel.endsWith(u'*') ? 1 : 0).toDouble());
        }
        if (!aok) allAbsolute = false;
        r.skipCurrentElement();
    };
    auto readRow = [&](bool inHeader) {
        const auto a = r.attributes();
        int rep = a.hasAttribute(ns::table, u"number-rows-repeated")
                      ? qMax(1, a.value(ns::table, u"number-rows-repeated").toInt()) : 1;
        Row row;
        const Style rs = m_styles.resolve(QStringLiteral("table-row"), a.value(ns::table, u"style-name").toString());
        row.height = toPoints(rs.row.value(QStringLiteral("style:min-row-height")));
        bool hasContent = false;
        while (r.readNextStartElement()) {
            const auto n = r.name();
            const bool covered = n == u"covered-table-cell";
            if (n != u"table-cell" && !covered) {
                r.skipCurrentElement();
                continue;
            }
            const auto ca = r.attributes();
            const int crep = qBound(1, ca.hasAttribute(ns::table, u"number-columns-repeated")
                                           ? ca.value(ns::table, u"number-columns-repeated").toInt() : 1,
                                    Limits::kMaxTableColumns);
            Cell cell;
            cell.covered = covered;
            cell.colSpan = qBound(1, ca.hasAttribute(ns::table, u"number-columns-spanned")
                                         ? ca.value(ns::table, u"number-columns-spanned").toInt() : 1,
                                  Limits::kMaxTableColumns);
            cell.rowSpan = qBound(1, ca.hasAttribute(ns::table, u"number-rows-spanned")
                                         ? ca.value(ns::table, u"number-rows-spanned").toInt() : 1,
                                  Limits::kMaxTableRows);
            const Style cs = m_styles.resolve(QStringLiteral("table-cell"), ca.value(ns::table, u"style-name").toString());
            cell.background = toColor(cs.cell.value(QStringLiteral("fo:background-color")));
            const Border all = parseBorder(cs.cell.value(QStringLiteral("fo:border")));
            auto side = [&](const char *key) {
                const QString v = cs.cell.value(QString::fromLatin1(key));
                return v.isEmpty() ? all : parseBorder(v);
            };
            cell.top = side("fo:border-top");
            cell.bottom = side("fo:border-bottom");
            cell.left = side("fo:border-left");
            cell.right = side("fo:border-right");
            const QString pad = cs.cell.value(QStringLiteral("fo:padding"));
            if (!pad.isEmpty()) cell.padding = qBound(0.0, toPoints(pad), 36.0);
            else if (cs.cell.contains(QStringLiteral("fo:padding-left")))
                cell.padding = qBound(0.0, toPoints(cs.cell.value(QStringLiteral("fo:padding-left"))), 36.0);
            const QString va = cs.cell.value(QStringLiteral("style:vertical-align"));
            if (va == u"middle") cell.vAlign = Qt::AlignVCenter;
            else if (va == u"bottom") cell.vAlign = Qt::AlignBottom;
            parseBlocks(r, cell.blocks, depth + 2);
            if (!cell.blocks.isEmpty()) hasContent = true;
            for (int i = 0; i < crep && row.cells.size() < Limits::kMaxTableColumns; ++i) row.cells << cell;
        }
        // Clamp huge repeats: empty repeated rows collapse to one.
        if (rep > 1) rep = hasContent ? qMin(rep, Limits::kMaxRepeat) : qMin(rep, 1);
        for (int i = 0; i < rep && m_rowsBudget > 0; ++i, --m_rowsBudget) {
            if (m_cellBudget < row.cells.size()) {
                m_doc.report.count(QStringLiteral("Table rows dropped (too many cells)"));
                m_rowsBudget = 0;
                break;
            }
            m_cellBudget -= int(row.cells.size());
            t.rows << row;
            if (inHeader) ++headerRows;
        }
    };
    // Row/column groups nest; the depth is bounded like all XML recursion.
    readContainer = [&](int nesting, bool inHeader) {
        while (r.readNextStartElement()) {
            const auto n = r.name();
            const bool group = n == u"table-columns" || n == u"table-header-columns" ||
                               n == u"table-column-group" || n == u"table-header-rows" ||
                               n == u"table-rows" || n == u"table-row-group";
            if (group && depth + nesting >= Limits::kMaxXmlDepth) {
                skip(r, QStringLiteral("Deeply nested content skipped"));
            } else if (n == u"table-column") {
                readColumn();
            } else if (n == u"table-row") {
                readRow(inHeader);
            } else if (group) {
                readContainer(nesting + 1, inHeader || n == u"table-header-rows");
            } else {
                r.skipCurrentElement();
            }
        }
    };
    readContainer(0, false);
    t.headerRows = headerRows;
    if (!allAbsolute) {
        // Relative widths: proportional split.
        t.columnWidths = relWidths;
        t.width = 0;
    } else if (t.width <= 0) {
        double sum = 0;
        for (double w : t.columnWidths) sum += w;
        if (sum > 0) t.width = 0;  // percentages computed by builder from absolutes
    }
    // Pad short rows so the grid is complete.
    const int cols = t.columnCount();
    for (Row &row : t.rows) {
        int c = int(row.cells.size());
        while (c < cols) {
            Cell filler;
            filler.covered = false;
            row.cells << filler;
            ++c;
        }
    }
    if (!t.rows.isEmpty()) out << Block::tableBlock(std::move(t));
}

void Importer::finish()
{
    // Choose the master page: the one named by the first paragraph, else
    // "Standard"/"Default", else the first.
    const MasterPage *mp = nullptr;
    for (const MasterPage &m : std::as_const(m_masters))
        if (!m_firstMaster.isEmpty() && m.name == m_firstMaster) mp = &m;
    for (const MasterPage &m : std::as_const(m_masters))
        if (!mp && (m.name == u"Standard" || m.name == u"Default")) mp = &m;
    if (!mp && !m_masters.isEmpty()) mp = &m_masters.first();
    if (!mp) return;

    PageSetup &ps = m_doc.page;
    if (const PageLayout *pl = m_styles.pageLayout(mp->layout)) {
        const Props &p = pl->page;
        bool okW = false, okH = false;
        const double w = toPoints(p.value(QStringLiteral("fo:page-width")), &okW);
        const double h = toPoints(p.value(QStringLiteral("fo:page-height")), &okH);
        if (okW && okH && w > 36 && h > 36) ps.size = SafeImage::sanitizePageSize(QSizeF(w, h), ps.size);
        auto margin = [&](const char *k, double def) {
            bool ok = false;
            const double v = toPoints(p.value(QString::fromLatin1(k)), &ok);
            return ok ? qMax(0.0, v) : def;
        };
        const double all = margin("fo:margin", 56.7);
        const double maxH = ps.size.width() / 2.5, maxV = ps.size.height() / 2.5;
        ps.margins = QMarginsF(qMin(margin("fo:margin-left", all), maxH), qMin(margin("fo:margin-top", all), maxV),
                               qMin(margin("fo:margin-right", all), maxH), qMin(margin("fo:margin-bottom", all), maxV));
        ps.background = toColor(p.value(QStringLiteral("fo:background-color")));
        if (p.value(QStringLiteral("style:columns")).toInt() > 1 ||
            p.value(QStringLiteral("fo:column-count")).toInt() > 1)
            m_doc.report.note(QStringLiteral("Multiple columns shown as one column"));
        ps.headerInsideMargins = true;
        ps.headerSpacing = toPoints(pl->header.value(QStringLiteral("fo:margin-bottom")));
        ps.footerSpacing = toPoints(pl->footer.value(QStringLiteral("fo:margin-top")));
    }
    m_doc.header = mp->header;
    m_doc.footer = mp->footer;
    if (mp->hasFirst) {
        m_doc.titlePage = true;
        m_doc.headerFirst = mp->headerFirst;
        m_doc.footerFirst = mp->footerFirst;
    }
    // Default character style from the default paragraph style.
    const ResolvedPara def = m_styles.resolveParagraph(QStringLiteral("Standard"));
    const CharStyle dc = m_styles.toCharStyle(def.textProps);
    m_doc.defaultChar.family = dc.family.isEmpty() ? QStringLiteral("Tinos") : dc.family;
    m_doc.defaultChar.size = dc.size > 0 ? dc.size : 12;
    if (m_masters.size() > 1) {
        bool differing = false;
        for (const MasterPage &m : std::as_const(m_masters))
            if (m.layout != mp->layout && (m.name != u"Standard")) differing = true;
        if (differing) m_doc.report.note(QStringLiteral("Mixed page layouts shown with one page size"));
    }
}

}  // namespace

Document importOdt(const QString &path, bool flat, QString *error)
{
    Document doc;
    if (flat) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() > qint64(Limits::kMaxEntryBytes)) {
            *error = QStringLiteral("Cannot read file");
            return doc;
        }
        Importer im(nullptr, doc);
        QString err;
        if (!im.parse(f.readAll(), &err)) doc.report.error(err);
        im.finish();
        return doc;
    }

    OdfPackage pkg;
    if (!pkg.open(path, error)) return doc;
    if (!pkg.mimeType().startsWith(QLatin1String("application/vnd.oasis.opendocument.text")))
        doc.report.note(QStringLiteral("Unexpected package type: %1").arg(pkg.mimeType()));
    Importer im(&pkg, doc);
    QString err;
    for (const QString &part : {QStringLiteral("styles.xml"), QStringLiteral("content.xml"),
                                QStringLiteral("meta.xml")}) {
        if (!pkg.contains(part)) continue;
        QString readErr;
        const QByteArray xml = pkg.read(part, &readErr);
        if (xml.isEmpty()) {
            doc.report.error(readErr);
            continue;
        }
        if (!im.parse(xml, &err)) doc.report.error(QStringLiteral("%1: %2").arg(part, err));
    }
    im.finish();

    if (doc.body.isEmpty() && pkg.contains(QStringLiteral("Thumbnails/thumbnail.png"))) {
        // Degraded preview: the thumbnail the authoring app stored.
        SafeImage::Budget budget(Limits::kMaxDocumentImageBytes);
        const QImage thumb = SafeImage::decodeEmbedded(pkg.read(QStringLiteral("Thumbnails/thumbnail.png")), budget);
        if (!thumb.isNull()) {
            doc.images.insert(QStringLiteral("thumbnail"), thumb);
            Paragraph p;
            p.style.align = Qt::AlignHCenter;
            Run r;
            r.imageKey = QStringLiteral("thumbnail");
            const double w = doc.page.size.width() - doc.page.margins.left() - doc.page.margins.right();
            r.imageSize = QSizeF(w, w * thumb.height() / qMax(1, thumb.width()));
            p.runs << r;
            doc.body << Block::paragraph(p);
            doc.report.error(QStringLiteral("Could not read the document; showing its stored thumbnail"));
        }
    }
    return doc;
}

}  // namespace odf
