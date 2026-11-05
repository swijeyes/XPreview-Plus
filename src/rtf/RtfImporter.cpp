// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "RtfImporter.h"

#include "RtfTokenizer.h"
#include "core/FontSubstitution.h"
#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QLocale>
#include <QMutex>
#include <QRegularExpression>
#include <QStringDecoder>

#include <climits>
#include <list>
#include <map>
#include <set>

namespace rtf {

namespace {

using namespace flow;

constexpr double kTwip = 1.0 / 20.0;
// Table geometry in twips is clamped to the largest page so integer
// arithmetic on cell edges cannot overflow.
constexpr int kMaxTwips = int(Limits::kMaxPageSide * 20);
inline int clampTwips(int v) { return qBound(-kMaxTwips, v, kMaxTwips); }

enum class Dest {
    Body,        // text goes to the current sink
    Skip,        // drop text
    FontTable,
    ColorTable,
    StyleSheet,
    ListTable,
    ListLevel,
    LevelText,
    ListOverride,
    Info,        // inside \info; sub-destinations carry the key
    InfoField,
    FieldInst,
    Pict,
};

struct Font {
    QString name;
    int codepage = 0;  // 0 = document default
    bool symbol = false;
};

struct ListLevelDef {
    int nfc = 0;
    int start = 1;
    QString levelText;  // decoded \leveltext (first char = length)
    QList<int> numberPositions;
    double indent = 0;
};

struct BorderTarget {
    enum Side { None, CellTop, CellLeft, CellBottom, CellRight, Para } side = None;
};

struct CellDef {
    int right = 0;
    bool hMergeFirst = false, hMerge = false, vMergeFirst = false, vMerge = false;
    int shading = -1;
    Border top, left, bottom, right_;
    Qt::Alignment vAlign = Qt::AlignTop;
};

struct RowDef {
    QList<CellDef> cells;
    CellDef pending;
    int left = 0;
    bool header = false;
    Qt::Alignment align = Qt::AlignLeft;
    double height = 0;
};

struct BuiltRow {
    RowDef def;
    QList<Blocks> cells;
};

// Where paragraphs end up: the body, a header/footer or a footnote.
struct Sink {
    Blocks *out = nullptr;
    Paragraph para;
    bool paraHasContent = false;
    QList<BuiltRow> rows;
    QList<Blocks> rowCells;
    Blocks cellBlocks;
};

struct CharState {
    int font = -1;
    int fontSize = 24;  // half-points
    bool bold = false, italic = false, underline = false, strike = false;
    bool smallCaps = false, caps = false, hidden = false;
    int vertical = 0;
    int fg = 0, bg = 0, highlight = 0;
    double letterSpacing = 0;
};

struct ParaState {
    ParaStyle style;
    bool inTable = false;
    int ls = 0, ilvl = 0;
    int styleIndex = -1;
    TabStop::Type nextTabType = TabStop::Left;
};

struct Group {
    Dest dest = Dest::Body;
    CharState chr;
    ParaState para;
    int uc = 1;
    Sink *sink = nullptr;
    QString href;
    bool uprSkipChild = false;
    QString infoKey;
    bool listTextSkip = false;
    int pictIndex = -1;  // which picture this group owns, if any
    bool fieldResult = false;
};

struct Pict {
    enum Type { Unknown, Png, Jpeg, Wmf, Emf, MacPict, Dib, Bitmap } type = Unknown;
    int picw = 0, pich = 0, goalw = 0, goalh = 0;
    int scalex = 100, scaley = 100;
    int cropl = 0, cropr = 0, cropt = 0, cropb = 0;
    QByteArray data;
    bool highNibble = true;
    unsigned char pending = 0;
};

struct FieldState {
    QString instruction;
};

class Interpreter {
public:
    explicit Interpreter(const QByteArray &data) : m_data(data), m_tok(data) {}
    Document run();

private:
    // Token handlers
    void controlWord(const Token &t);
    void controlSymbol(char c);
    void groupStart();
    void groupEnd();
    void bytes(const QByteArray &b);
    void flushBytes();
    void text(const QString &s);

    // Output
    CharStyle currentCharStyle() const;
    void emitRun(const QString &s);
    void emitRun(Run run);
    void endParagraph();
    void endCell();
    void endRow();
    void flushTable(Sink &sink);
    void finishSink(Sink &sink);
    void pageBreak() { m_pendingPageBreak = true; }
    void finishPict(int index);
    ListRef listRefFor(const ParaState &ps);
    QString decode(const QByteArray &b, int codepage);
    int effectiveCodepage() const;

    Group &g() { return m_stack.back(); }
    const Group &g() const { return m_stack.back(); }

    const QByteArray &m_data;
    Tokenizer m_tok;
    Document m_doc;
    std::vector<Group> m_stack;
    std::list<Sink> m_sinks;  // stable addresses
    Sink *m_body = nullptr;

    QHash<int, Font> m_fonts;
    int m_currentFontEntry = -1;
    QByteArray m_fontNameBytes;
    QList<QColor> m_colors;
    int m_r = -1, m_gc = -1, m_b = -1;
    QHash<int, QString> m_styleNames;
    int m_currentStyleEntry = -1;
    QString m_styleNameAcc;
    QHash<int, int> m_styleHeading;

    QHash<int, QList<ListLevelDef>> m_lists;  // listid -> levels
    int m_currentListId = 0;
    QList<ListLevelDef> m_buildingLevels;
    ListLevelDef m_buildingLevel;
    QHash<int, int> m_overrides;  // ls -> listid
    int m_ovListId = 0, m_ovLs = 0;

    QByteArray m_pendingBytes;
    int m_pendingSkip = 0;
    int m_docCodepage = 1252;
    int m_defaultFont = -1;
    bool m_pendingPageBreak = false;
    bool m_sectBreakPage = true;
    int m_footnoteCounter = 0;
    bool m_starPending = false;
    std::vector<Pict> m_picts;
    std::vector<FieldState> m_fields;
    BorderTarget m_border;
    RowDef m_rowDef;
    int m_imageCounter = 0;
    SafeImage::Budget m_imageBudget{Limits::kMaxDocumentImageBytes};
    int m_cellBudget = Limits::kMaxTableCells;

    // Page setup in twips.
    int m_paperW = 0, m_paperH = 0, m_margL = 1440, m_margR = 1440, m_margT = 1440, m_margB = 1440;
    int m_headerY = 720, m_footerY = 720;
    bool m_landscape = false;
    bool m_sawPaper = false;
    bool m_inFirstSection = true;
    std::map<std::string, QString> m_info;
};

const std::set<QByteArray> &skippedDestinations()
{
    static const std::set<QByteArray> s = {
        "filetbl", "revtbl", "rsidtbl", "pgdsctbl", "template", "themedata", "colorschememapping",
        "datastore", "latentstyles", "xmlnstbl", "objdata", "pnseclvl", "bkmkstart", "bkmkend",
        "atnid", "atnauthor", "annotation", "atndate", "atrfstart", "atrfend", "xe", "tc", "tcn",
        "txe", "rxe", "generator", "nonshppict", "pn", "picprop", "listpicture", "fontemb",
        "fontfile", "falt", "panose", "mmathPr", "ftnsep", "ftnsepc", "ftncn", "aftnsep",
        "aftnsepc", "aftncn", "docvar", "userprops", "wgrffmtfilter", "passwordhash", "printim",
        "buptim", "creatim", "revtim", "hlinkbase", "listname", "levelnumbers", "shpinst",
        "sp", "sn", "sv", "do", "headerl", "footerl", "private", "nesttableprops",
        "expandedcolortbl", "pgptbl", "factoidname", "oldcprops", "oldpprops", "oldtprops",
        "oldsprops", "background", "mhtmltag", "htmltag", "lsdlockedexcept", "fchars", "lchars",
        "macpict", "pmmetafile", "dptxbxtext", "formfield", "datafield", "ud_skip",
    };
    return s;
}

int charsetToCodepage(int charset)
{
    switch (charset) {
    case 0: return 1252;
    case 77: return 10000;
    case 128: return 932;
    case 129: return 949;
    case 130: return 1361;
    case 134: return 936;
    case 136: return 950;
    case 161: return 1253;
    case 162: return 1254;
    case 163: return 1258;
    case 177: return 1255;
    case 178: return 1256;
    case 186: return 1257;
    case 204: return 1251;
    case 222: return 874;
    case 238: return 1250;
    case 254: return 437;
    case 255: return 850;
    default: return 0;
    }
}

QByteArray codecName(int cp)
{
    switch (cp) {
    case 10000: return "macintosh";
    case 932: return "Shift_JIS";
    case 936: return "GBK";
    case 949: return "windows-949";
    case 950: return "Big5";
    case 437: return "IBM437";
    case 850: return "IBM850";
    case 65001: return "UTF-8";
    default: return "windows-" + QByteArray::number(cp);
    }
}

bool sameStyle(const CharStyle &a, const CharStyle &b)
{
    return a.family == b.family && a.size == b.size && a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.strike == b.strike && a.smallCaps == b.smallCaps &&
           a.allCaps == b.allCaps && a.vertical == b.vertical && a.color == b.color &&
           a.background == b.background && a.letterSpacing == b.letterSpacing &&
           a.hidden == b.hidden;
}

ListMarker markerForNfc(int nfc)
{
    switch (nfc) {
    case 0: return ListMarker::Decimal;
    case 1: return ListMarker::UpperRoman;
    case 2: return ListMarker::LowerRoman;
    case 3: return ListMarker::UpperAlpha;
    case 4: return ListMarker::LowerAlpha;
    case 23: return ListMarker::Text;
    case 255: return ListMarker::None;
    default: return ListMarker::Decimal;
    }
}

QString parseHyperlink(const QString &inst)
{
    static const QRegularExpression re(QStringLiteral(R"(HYPERLINK\s+(?:\\l\s+)?"([^"]*)\")"));
    const auto m = re.match(inst);
    if (m.hasMatch()) {
        const bool local = inst.contains(QLatin1String("\\l"));
        return local ? QLatin1Char('#') + m.captured(1) : m.captured(1);
    }
    static const QRegularExpression bare(QStringLiteral(R"(HYPERLINK\s+(\S+))"));
    const auto b = bare.match(inst);
    return b.hasMatch() ? b.captured(1) : QString();
}

Document Interpreter::run()
{
    m_sinks.emplace_back();
    m_body = &m_sinks.back();
    m_body->out = &m_doc.body;
    Group root;
    root.sink = m_body;
    m_stack.push_back(root);

    qint64 tokens = 0;
    bool sawRtf = false;
    for (;;) {
        Token t = m_tok.next();
        if (t.type == Token::End) break;
        if (++tokens > Limits::kMaxRtfTokens) {
            m_doc.report.error(QStringLiteral("Document truncated: too many RTF tokens"));
            break;
        }
        if (t.type != Token::Text && t.type != Token::HexByte) flushBytes();
        switch (t.type) {
        case Token::GroupStart: groupStart(); break;
        case Token::GroupEnd: groupEnd(); break;
        case Token::ControlWord:
            if (t.word == "rtf") sawRtf = true;
            controlWord(t);
            break;
        case Token::ControlSymbol: controlSymbol(t.symbol); break;
        case Token::Text: bytes(t.word); break;
        case Token::HexByte:
            if (g().dest == Dest::Pict) {
                // Hex escapes inside pict data are unusual; treat as data.
                if (g().pictIndex >= 0 && m_picts[g().pictIndex].data.size() < Limits::kMaxPictBytes)
                    m_picts[g().pictIndex].data.append(char(t.hex));
                break;
            }
            if (m_pendingSkip > 0) {
                --m_pendingSkip;
                break;
            }
            m_pendingBytes.append(char(t.hex));
            break;
        case Token::Binary:
            if (g().dest == Dest::Pict && g().pictIndex >= 0) {
                Pict &p = m_picts[g().pictIndex];
                if (p.data.size() + t.binary.size() <= Limits::kMaxPictBytes) p.data.append(t.binary);
            }
            break;
        case Token::End:
        case Token::Skip: break;
        }
    }
    flushBytes();
    if (!sawRtf) m_doc.report.error(QStringLiteral("Missing \\rtf header"));
    while (m_stack.size() > 1) groupEnd();  // unbalanced braces: close them
    for (Sink &s : m_sinks) finishSink(s);

    // Page setup
    PageSetup &ps = m_doc.page;
    // TextEdit and many generators omit page setup: use the locale's paper.
    const QSizeF localePaper = QLocale::system().measurementSystem() == QLocale::ImperialUSSystem
                                   ? QSizeF(612, 792)
                                   : QSizeF(595.276, 841.89);
    ps.size = localePaper;
    if (m_sawPaper && m_paperW > 0 && m_paperH > 0) {
        ps.size = SafeImage::sanitizePageSize(QSizeF(m_paperW * kTwip, m_paperH * kTwip), localePaper);
        if (m_landscape && ps.size.width() < ps.size.height()) ps.size.transpose();
    }
    // Margins and header distances must leave the page usable.
    auto margin = [&](int twips, double side) { return qBound(0.0, twips * kTwip, side / 2.5); };
    ps.margins = QMarginsF(margin(m_margL, ps.size.width()), margin(m_margT, ps.size.height()),
                           margin(m_margR, ps.size.width()), margin(m_margB, ps.size.height()));
    ps.headerDistance = margin(m_headerY, ps.size.height());
    ps.footerDistance = margin(m_footerY, ps.size.height());

    if (m_defaultFont >= 0 && m_fonts.contains(m_defaultFont))
        m_doc.defaultChar.family = m_fonts[m_defaultFont].name;
    if (m_doc.defaultChar.family.isEmpty()) m_doc.defaultChar.family = QStringLiteral("Times New Roman");
    m_doc.defaultChar.size = 12;

    for (const auto &[k, v] : m_info) {
        if (!v.trimmed().isEmpty()) m_doc.metadata.insert(QString::fromStdString(k), v.trimmed());
    }
    return std::move(m_doc);
}

int Interpreter::effectiveCodepage() const
{
    const int f = g().chr.font;
    auto it = m_fonts.constFind(f);
    if (it != m_fonts.cend() && it->codepage > 0) return it->codepage;
    return m_docCodepage;
}

QString Interpreter::decode(const QByteArray &b, int codepage)
{
    if (b.isEmpty()) return {};
    bool ascii = true;
    for (char c : b)
        if (uchar(c) >= 0x80) { ascii = false; break; }
    if (ascii) return QString::fromLatin1(b);
    static QHash<int, std::shared_ptr<QStringDecoder>> decoders;
    static QMutex lock;
    QMutexLocker l(&lock);
    auto &dec = decoders[codepage];
    if (!dec) dec = std::make_shared<QStringDecoder>(codecName(codepage).constData());
    if (!dec->isValid()) return QString::fromLatin1(b);
    QString out = dec->decode(b);
    dec->resetState();
    return out;
}

void Interpreter::bytes(const QByteArray &b)
{
    QByteArray data = b;
    if (m_pendingSkip > 0) {
        const int n = qMin(m_pendingSkip, int(data.size()));
        data.remove(0, n);
        m_pendingSkip -= n;
    }
    if (data.isEmpty()) return;
    if (g().dest == Dest::Pict) {
        if (g().pictIndex < 0) return;
        Pict &p = m_picts[g().pictIndex];
        if (p.data.size() > Limits::kMaxPictBytes) return;
        for (char c : data) {
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (p.highNibble) {
                p.pending = uchar(v << 4);
                p.highNibble = false;
            } else {
                p.data.append(char(p.pending | v));
                p.highNibble = true;
            }
        }
        return;
    }
    m_pendingBytes.append(data);
}

void Interpreter::flushBytes()
{
    if (m_pendingBytes.isEmpty()) return;
    const QByteArray b = std::exchange(m_pendingBytes, {});
    text(decode(b, effectiveCodepage()));
}

void Interpreter::text(const QString &s)
{
    if (s.isEmpty()) return;
    Group &gr = g();
    switch (gr.dest) {
    case Dest::Body:
        emitRun(s);
        break;
    case Dest::FontTable:
        // Font names end at ';'. TextEdit puts every font in one group.
        for (QChar c : s) {
            if (c == QLatin1Char(';')) {
                if (m_currentFontEntry >= 0) {
                    Font &f = m_fonts[m_currentFontEntry];
                    f.name = QString::fromUtf8(m_fontNameBytes).trimmed();
                    if (f.name.isEmpty()) f.name = QString::fromLatin1(m_fontNameBytes).trimmed();
                }
                m_fontNameBytes.clear();
            } else {
                m_fontNameBytes.append(QString(c).toUtf8());
            }
        }
        break;
    case Dest::ColorTable:
        for (QChar c : s) {
            if (c != QLatin1Char(';')) continue;
            if (m_r < 0 && m_gc < 0 && m_b < 0)
                m_colors << QColor();  // auto
            else
                m_colors << QColor(qBound(0, m_r, 255), qBound(0, m_gc, 255), qBound(0, m_b, 255));
            m_r = m_gc = m_b = -1;
        }
        break;
    case Dest::StyleSheet:
        for (QChar c : s) {
            if (c == QLatin1Char(';')) {
                if (m_currentStyleEntry >= 0) {
                    const QString name = m_styleNameAcc.trimmed().toLower();
                    m_styleNames.insert(m_currentStyleEntry, name);
                    static const QRegularExpression re(QStringLiteral(R"(^heading\s*(\d))"));
                    const auto m = re.match(name);
                    if (m.hasMatch()) m_styleHeading.insert(m_currentStyleEntry, m.captured(1).toInt());
                    else if (name == QLatin1String("title")) m_styleHeading.insert(m_currentStyleEntry, 1);
                }
                m_styleNameAcc.clear();
            } else {
                m_styleNameAcc += c;
            }
        }
        break;
    case Dest::LevelText:
        m_buildingLevel.levelText += s;
        break;
    case Dest::InfoField:
        m_info[gr.infoKey.toStdString()] += s;
        break;
    case Dest::FieldInst:
        if (!m_fields.empty()) m_fields.back().instruction += s;
        break;
    default:
        break;
    }
}

CharStyle Interpreter::currentCharStyle() const
{
    const CharState &c = g().chr;
    CharStyle s;
    auto it = m_fonts.constFind(c.font);
    if (it != m_fonts.cend()) s.family = it->name;
    s.size = c.fontSize / 2.0;
    s.bold = c.bold;
    s.italic = c.italic;
    s.underline = c.underline;
    s.strike = c.strike;
    s.smallCaps = c.smallCaps;
    s.allCaps = c.caps;
    s.hidden = c.hidden;
    s.vertical = c.vertical;
    s.letterSpacing = c.letterSpacing;
    if (c.fg > 0 && c.fg < m_colors.size()) s.color = m_colors[c.fg];
    const int bg = c.highlight > 0 ? c.highlight : c.bg;
    if (bg > 0 && bg < m_colors.size()) s.background = m_colors[bg];
    return s;
}

void Interpreter::emitRun(const QString &s)
{
    Run r;
    r.style = currentCharStyle();
    r.href = g().href;
    QString t = s;
    auto it = m_fonts.constFind(g().chr.font);
    if (it != m_fonts.cend() && it->symbol) {
        for (QChar &c : t) c = FontSubstitution::mapSymbolChar(it->name, c);
        r.style.family.clear();  // the mapped glyphs are plain Unicode now
    }
    r.text = t;
    emitRun(std::move(r));
}

void Interpreter::emitRun(Run run)
{
    Sink *sink = g().sink;
    if (!sink) return;
    Paragraph &p = sink->para;
    if (!p.runs.isEmpty() && run.imageKey.isEmpty() && run.field == Field::None) {
        Run &last = p.runs.last();
        if (last.imageKey.isEmpty() && last.field == Field::None && last.href == run.href &&
            sameStyle(last.style, run.style)) {
            last.text += run.text;
            sink->paraHasContent = true;
            return;
        }
    }
    p.runs << std::move(run);
    sink->paraHasContent = true;
}

ListRef Interpreter::listRefFor(const ParaState &ps)
{
    ListRef ref;
    ref.listId = ps.ls;
    ref.level = qBound(0, ps.ilvl, Limits::kMaxListLevels - 1);
    const int listId = m_overrides.value(ps.ls, -1);
    const QList<ListLevelDef> levels = m_lists.value(listId);
    if (ref.level < levels.size()) {
        const ListLevelDef &def = levels[ref.level];
        ref.format.marker = markerForNfc(def.nfc);
        ref.format.start = def.start;
        ref.format.indent = def.indent;
        // \leveltext: first char is the length, \'0N are level placeholders.
        QString lt = def.levelText;
        if (lt.endsWith(QLatin1Char(';'))) lt.chop(1);
        if (!lt.isEmpty()) lt.remove(0, 1);
        if (ref.format.marker == ListMarker::Text) {
            ref.format.bulletText = lt;
        } else {
            int ph = -1;
            for (int i = 0; i < lt.size(); ++i)
                if (lt[i].unicode() == ushort(ref.level)) ph = i;
            if (ph >= 0) {
                QString prefix = lt.left(ph);
                // Drop placeholders for parent levels ("1.1." becomes ".").
                prefix.remove(QRegularExpression(QStringLiteral("[\\x00-\\x08]")));
                ref.format.prefix = prefix.size() <= 3 ? prefix : QString();
                ref.format.suffix = lt.mid(ph + 1);
            }
        }
    } else {
        ref.format.marker = ListMarker::Disc;
    }
    return ref;
}

void Interpreter::endParagraph()
{
    Sink *sink = g().sink;
    if (!sink) return;
    const ParaState &ps = g().para;
    Paragraph p = std::exchange(sink->para, {});
    sink->paraHasContent = false;
    p.style = ps.style;
    p.markStyle = currentCharStyle();
    if (m_pendingPageBreak) {
        p.style.pageBreakBefore = true;
        m_pendingPageBreak = false;
    }
    if (ps.styleIndex >= 0 && p.style.headingLevel == 0)
        p.style.headingLevel = m_styleHeading.value(ps.styleIndex, 0);
    if (ps.ls > 0) p.list = listRefFor(ps);
    if (ps.inTable) {
        sink->cellBlocks << Block::paragraph(std::move(p));
        return;
    }
    if (!sink->rows.isEmpty() || !sink->rowCells.isEmpty()) flushTable(*sink);
    sink->out->append(Block::paragraph(std::move(p)));
}

void Interpreter::endCell()
{
    Sink *sink = g().sink;
    if (!sink) return;
    if (sink->paraHasContent || sink->cellBlocks.isEmpty()) {
        ParaState saved = g().para;
        g().para.inTable = true;
        endParagraph();
        g().para = saved;
    }
    sink->rowCells << std::exchange(sink->cellBlocks, {});
}

void Interpreter::endRow()
{
    Sink *sink = g().sink;
    if (!sink) return;
    if (!sink->cellBlocks.isEmpty()) sink->rowCells << std::exchange(sink->cellBlocks, {});
    BuiltRow row;
    row.def = m_rowDef;
    row.cells = std::exchange(sink->rowCells, {});
    if (sink->rows.size() < Limits::kMaxTableRows) sink->rows << row;
}

void Interpreter::flushTable(Sink &sink)
{
    if (!sink.rowCells.isEmpty() || !sink.cellBlocks.isEmpty()) {
        if (!sink.cellBlocks.isEmpty()) sink.rowCells << std::exchange(sink.cellBlocks, {});
        BuiltRow row;
        row.def = m_rowDef;
        row.cells = std::exchange(sink.rowCells, {});
        sink.rows << row;
    }
    if (sink.rows.isEmpty()) return;
    const QList<BuiltRow> rows = std::exchange(sink.rows, {});

    // Build a column grid from the union of all cell boundaries.
    std::set<int> bounds;
    int left = INT_MAX;
    for (const BuiltRow &r : rows) {
        left = qMin(left, r.def.left);
        int prev = r.def.left;
        for (const CellDef &c : r.def.cells) {
            const int right = qMax(c.right, prev + 1);
            bounds.insert(right);
            prev = right;
        }
    }
    if (left == INT_MAX) left = 0;
    std::vector<int> edges(bounds.begin(), bounds.end());
    if (edges.size() > size_t(Limits::kMaxTableColumns)) edges.resize(Limits::kMaxTableColumns);
    Table t;
    t.leftIndent = qMax(0, left) * kTwip;
    int prevEdge = left;
    for (int e : edges) {
        t.columnWidths << qMax(1, e - prevEdge) * kTwip;
        prevEdge = e;
    }
    double total = 0;
    for (double w : t.columnWidths) total += w;
    t.width = total;
    const int cols = qMax<int>(1, int(edges.size()));
    if (edges.empty()) {
        // No \cellx at all: equal columns.
        int maxCells = 1;
        for (const BuiltRow &r : rows) maxCells = qMax(maxCells, int(r.cells.size()));
        t.columnWidths.clear();
        t.width = 0;
        for (int i = 0; i < maxCells; ++i) t.columnWidths << 100;
    }
    const int ncols = edges.empty() ? int(t.columnWidths.size()) : cols;
    t.align = rows.first().def.align;
    if (m_cellBudget <= 0) {
        m_doc.report.count(QStringLiteral("Tables dropped (document has too many cells)"));
        return;
    }

    auto colIndex = [&](int edge) {
        auto it = std::lower_bound(edges.begin(), edges.end(), edge);
        return int(std::min<size_t>(it - edges.begin(), edges.size() - 1));
    };
    for (const BuiltRow &br : rows) {
        if (m_cellBudget < ncols) {
            m_doc.report.count(QStringLiteral("Table rows dropped (too many cells)"));
            break;
        }
        m_cellBudget -= ncols;
        Row row;
        row.height = br.def.height;
        for (int c = 0; c < ncols; ++c) {
            Cell cell;
            cell.covered = false;
            row.cells << cell;
        }
        QList<bool> used(ncols, false);
        int prev = br.def.left;
        int col = 0;
        const int ndefs = edges.empty() ? int(br.cells.size()) : int(br.def.cells.size());
        for (int i = 0; i < ndefs && col < ncols; ++i) {
            CellDef def = i < br.def.cells.size() ? br.def.cells[i] : CellDef{};
            int endCol = col;
            if (!edges.empty()) {
                const int right = qMax(def.right, prev + 1);
                endCol = colIndex(right);
                prev = right;
            }
            endCol = qBound(col, endCol, ncols - 1);
            Cell &cell = row.cells[col];
            cell.colSpan = endCol - col + 1;
            for (int k = col + 1; k <= endCol; ++k) row.cells[k].covered = true;
            if (i < br.cells.size()) cell.blocks = br.cells[i];
            if (def.shading > 0 && def.shading < m_colors.size()) cell.background = m_colors[def.shading];
            cell.top = def.top;
            cell.left = def.left;
            cell.bottom = def.bottom;
            cell.right = def.right_;
            cell.vAlign = def.vAlign;
            cell.padding = 3;
            if (def.hMerge && col > 0) {
                // Merge into the nearest uncovered cell to the left.
                int k = col - 1;
                while (k > 0 && row.cells[k].covered) --k;
                row.cells[k].colSpan += cell.colSpan;
                for (const Block &b : std::as_const(cell.blocks))
                    if (b.kind != Block::Para || !b.para.runs.isEmpty()) row.cells[k].blocks << b;
                cell.covered = true;
            }
            if (def.vMerge) cell.rowSpan = -1;  // resolved below
            for (int k = col; k <= endCol; ++k) used[k] = true;
            col = endCol + 1;
        }
        // Extra content cells beyond the definitions go into the last cell.
        for (int i = ndefs; i < br.cells.size(); ++i)
            if (col > 0) row.cells[col - 1].blocks << br.cells[i];
        for (int k = 0; k < ncols; ++k)
            if (!used[k] && !row.cells[k].covered) row.cells[k].blocks.clear();
        if (br.def.header && t.rows.size() == t.headerRows) ++t.headerRows;
        t.rows << row;
    }
    // Vertical merges: continuation cells extend the cell above.
    for (int r = 1; r < t.rows.size(); ++r) {
        for (int c = 0; c < ncols; ++c) {
            Cell &cell = t.rows[r].cells[c];
            if (cell.rowSpan != -1) continue;
            cell.rowSpan = 1;
            int above = r - 1;
            while (above > 0 && t.rows[above].cells[c].covered) --above;
            Cell &top = t.rows[above].cells[c];
            if (top.covered) continue;
            top.rowSpan = r - above + 1;
            for (const Block &b : std::as_const(cell.blocks))
                if (b.kind != Block::Para || !b.para.runs.isEmpty()) top.blocks << b;
            cell.covered = true;
            for (int k = c + 1; k < c + cell.colSpan && k < ncols; ++k) t.rows[r].cells[k].covered = true;
        }
    }
    for (Row &row : t.rows)
        for (Cell &cell : row.cells)
            if (cell.rowSpan < 1) cell.rowSpan = 1;
    sink.out->append(Block::tableBlock(std::move(t)));
}

void Interpreter::finishSink(Sink &sink)
{
    if (sink.paraHasContent) {
        Paragraph p = std::exchange(sink.para, {});
        sink.paraHasContent = false;
        if (!sink.rows.isEmpty() || !sink.rowCells.isEmpty()) flushTable(sink);
        sink.out->append(Block::paragraph(std::move(p)));
    }
    if (!sink.rows.isEmpty() || !sink.rowCells.isEmpty() || !sink.cellBlocks.isEmpty())
        flushTable(sink);
}

void Interpreter::groupStart()
{
    if (int(m_stack.size()) >= Limits::kMaxRtfGroupDepth) {
        m_doc.report.count(QStringLiteral("Deeply nested RTF groups skipped"));
        m_tok.skipGroup();
        return;
    }
    if (g().uprSkipChild) {
        // \upr: the first child is the ANSI version; \ud holds the Unicode one.
        g().uprSkipChild = false;
        m_tok.skipGroup();
        return;
    }
    Group child = g();
    child.uprSkipChild = false;
    child.pictIndex = -1;
    child.fieldResult = false;
    m_stack.push_back(child);
    m_pendingSkip = 0;
}

void Interpreter::groupEnd()
{
    if (m_stack.size() <= 1) return;  // unbalanced: ignore extra '}'
    Group ended = m_stack.back();
    if (ended.dest == Dest::FontTable && m_currentFontEntry >= 0 && !m_fontNameBytes.isEmpty()) {
        // Entry without trailing ';'.
        m_fonts[m_currentFontEntry].name = QString::fromUtf8(m_fontNameBytes).trimmed();
        m_fontNameBytes.clear();
    }
    m_stack.pop_back();
    m_pendingSkip = 0;
    if (ended.pictIndex >= 0) finishPict(ended.pictIndex);
    if (ended.dest == Dest::ListLevel && g().dest != Dest::ListLevel) {
        m_buildingLevels << m_buildingLevel;
        m_buildingLevel = {};
    }
    if (ended.dest == Dest::ListTable && g().dest == Dest::ListTable && !m_buildingLevels.isEmpty()) {
        // End of one \list group.
        m_lists.insert(m_currentListId, std::exchange(m_buildingLevels, {}));
    }
    if (ended.dest == Dest::ListOverride && g().dest == Dest::ListOverride) {
        if (m_ovLs > 0) m_overrides.insert(m_ovLs, m_ovListId);
        m_ovLs = m_ovListId = 0;
    }
    if (ended.fieldResult && !m_fields.empty()) m_fields.pop_back();
    if (ended.sink != g().sink && ended.sink) finishSink(*ended.sink);
}

void Interpreter::finishPict(int index)
{
    Pict &p = m_picts[index];
    QImage img;
    bool refused = false;
    if ((p.type == Pict::Png || p.type == Pict::Jpeg || p.type == Pict::Unknown) && !p.data.isEmpty())
        img = SafeImage::decodeEmbedded(p.data, m_imageBudget, &refused);
    if (refused) m_doc.report.count(QStringLiteral("Images too large to show"));
    Run r;
    r.style = currentCharStyle();
    if (img.isNull()) {
        QString label;
        switch (p.type) {
        case Pict::Wmf: label = QStringLiteral("WMF picture"); break;
        case Pict::Emf: label = QStringLiteral("EMF picture"); break;
        case Pict::MacPict: label = QStringLiteral("PICT picture"); break;
        default: label = QStringLiteral("Picture"); break;
        }
        m_doc.report.count(QStringLiteral("Pictures shown as placeholders"));
        r.text = QStringLiteral("[%1]").arg(label);
        r.style.color = QColor(120, 120, 120);
        r.style.italic = true;
        emitRun(std::move(r));
        p.data.clear();
        return;
    }
    double w = 0, h = 0;
    if (p.goalw > 0 && p.goalh > 0) {
        w = p.goalw * kTwip;
        h = p.goalh * kTwip;
    } else {
        w = img.width() * 0.75;  // assume 96 dpi
        h = img.height() * 0.75;
    }
    w *= p.scalex / 100.0;
    h *= p.scaley / 100.0;
    const QString key = QStringLiteral("rtf%1").arg(++m_imageCounter);
    m_doc.images.insert(key, img);
    r.imageKey = key;
    r.imageSize = QSizeF(qBound(1.0, w, 5000.0), qBound(1.0, h, 5000.0));
    emitRun(std::move(r));
    p.data.clear();
}

void Interpreter::controlSymbol(char c)
{
    switch (c) {
    case '*': m_starPending = true; return;
    case '\\': case '{': case '}': text(QString(QLatin1Char(c))); break;
    case '~': text(QString(QChar(0x00A0))); break;
    case '-': text(QString(QChar(0x00AD))); break;
    case '_': text(QString(QChar(0x2011))); break;
    case '|': case ':': break;
    case '\t': text(QStringLiteral("\t")); break;
    default: break;
    }
    m_starPending = false;
}

void Interpreter::controlWord(const Token &t)
{
    const QByteArray &w = t.word;
    const int p = t.param;
    const bool on = !t.hasParam || p != 0;
    Group &gr = g();
    const bool star = std::exchange(m_starPending, false);

    // A control word other than \uN ends a pending \uc fallback skip.
    if (w != "u") m_pendingSkip = 0;

    // --- destinations ---------------------------------------------------
    auto beginSink = [&](Blocks *target) {
        m_sinks.emplace_back();
        Sink &s = m_sinks.back();
        s.out = target;
        gr.sink = &s;
        gr.dest = Dest::Body;
        gr.para = ParaState{};
    };
    if (gr.dest == Dest::Skip) return;

    if (w == "fonttbl") { gr.dest = Dest::FontTable; return; }
    if (w == "colortbl") { gr.dest = Dest::ColorTable; m_colors.clear(); return; }
    if (w == "stylesheet") { gr.dest = Dest::StyleSheet; return; }
    if (w == "info") { gr.dest = Dest::Info; return; }
    if (gr.dest == Dest::Info) {
        static const std::set<QByteArray> keys = {"title", "author", "subject", "keywords",
                                                  "company", "operator", "doccomm", "comment"};
        if (keys.count(w)) {
            gr.dest = Dest::InfoField;
            gr.infoKey = QString::fromLatin1(w);
            gr.infoKey[0] = gr.infoKey[0].toUpper();
        } else {
            gr.dest = Dest::Skip;
        }
        return;
    }
    if (w == "listtable") { gr.dest = Dest::ListTable; return; }
    if (w == "listoverridetable") { gr.dest = Dest::ListOverride; return; }
    if (gr.dest == Dest::ListTable || gr.dest == Dest::ListLevel || gr.dest == Dest::LevelText) {
        if (w == "listlevel") { gr.dest = Dest::ListLevel; m_buildingLevel = {}; return; }
        if (w == "leveltext") { gr.dest = Dest::LevelText; m_buildingLevel.levelText.clear(); return; }
        if (w == "levelnfc" || w == "levelnfcn") { m_buildingLevel.nfc = p; return; }
        if (w == "levelstartat") { m_buildingLevel.start = p; return; }
        if (w == "li" || w == "lin") { m_buildingLevel.indent = p * kTwip; return; }
        if (w == "listid") { m_currentListId = p; return; }
        if (w == "u" && gr.dest == Dest::LevelText) {
            flushBytes();
            m_buildingLevel.levelText += QChar(ushort(p < 0 ? p + 65536 : p));
            m_pendingSkip = gr.uc;
            return;
        }
        if (w == "listname" || w == "levelnumbers") { gr.dest = Dest::Skip; return; }
        return;  // ignore other list formatting words
    }
    if (gr.dest == Dest::ListOverride) {
        if (w == "listid") m_ovListId = p;
        else if (w == "ls") m_ovLs = p;
        return;
    }
    if (gr.dest == Dest::FontTable) {
        if (w == "f") { m_currentFontEntry = p; m_fonts[p]; m_fontNameBytes.clear(); }
        else if (w == "fcharset") {
            Font &f = m_fonts[m_currentFontEntry];
            f.codepage = charsetToCodepage(p);
            f.symbol = p == 2;
        } else if (w == "cpg" && p > 0) m_fonts[m_currentFontEntry].codepage = p;
        else if (skippedDestinations().count(w) || star) gr.dest = Dest::Skip;
        return;
    }
    if (gr.dest == Dest::ColorTable) {
        if (w == "red") m_r = p;
        else if (w == "green") m_gc = p;
        else if (w == "blue") m_b = p;
        return;
    }
    if (gr.dest == Dest::StyleSheet) {
        if (w == "s") { m_currentStyleEntry = p; m_styleNameAcc.clear(); }
        else if (w == "cs" || w == "ds" || w == "ts") { m_currentStyleEntry = -1; m_styleNameAcc.clear(); }
        else if (star || skippedDestinations().count(w)) gr.dest = Dest::Skip;
        return;
    }
    if (gr.dest == Dest::Pict) {
        if (gr.pictIndex < 0) return;
        Pict &pi = m_picts[gr.pictIndex];
        if (w == "pngblip") pi.type = Pict::Png;
        else if (w == "jpegblip") pi.type = Pict::Jpeg;
        else if (w == "wmetafile") pi.type = Pict::Wmf;
        else if (w == "emfblip") pi.type = Pict::Emf;
        else if (w == "macpict") pi.type = Pict::MacPict;
        else if (w == "dibitmap") pi.type = Pict::Dib;
        else if (w == "wbitmap") pi.type = Pict::Bitmap;
        else if (w == "picw") pi.picw = p;
        else if (w == "pich") pi.pich = p;
        else if (w == "picwgoal") pi.goalw = p;
        else if (w == "pichgoal") pi.goalh = p;
        else if (w == "picscalex") pi.scalex = qBound(1, p, 1000);
        else if (w == "picscaley") pi.scaley = qBound(1, p, 1000);
        else if (star || skippedDestinations().count(w)) gr.dest = Dest::Skip;
        return;
    }
    if (gr.dest == Dest::FieldInst) {
        if (star || skippedDestinations().count(w)) gr.dest = Dest::Skip;
        else if (w == "u") {
            if (!m_fields.empty()) m_fields.back().instruction += QChar(ushort(p < 0 ? p + 65536 : p));
            m_pendingSkip = gr.uc;
        }
        return;
    }

    if (w == "pict") {
        gr.dest = Dest::Pict;
        m_picts.emplace_back();
        gr.pictIndex = int(m_picts.size()) - 1;
        return;
    }
    if (w == "shppict") return;  // preferred picture container: process
    if (w == "nonshppict") { gr.dest = Dest::Skip; return; }
    if (w == "shp") {
        m_doc.report.count(QStringLiteral("Drawing objects not shown"));
        gr.dest = Dest::Skip;
        return;
    }
    if (w == "object") { m_doc.report.count(QStringLiteral("Embedded objects shown as their preview")); return; }
    if (w == "result" || w == "shprslt") return;
    if (w == "field") { m_fields.push_back({}); gr.fieldResult = true; return; }
    if (w == "fldinst") { gr.dest = Dest::FieldInst; return; }
    if (w == "fldrslt") {
        const QString inst = m_fields.empty() ? QString() : m_fields.back().instruction.trimmed();
        const QString upper = inst.section(QLatin1Char(' '), 0, 0).toUpper();
        if (upper == QLatin1String("HYPERLINK")) {
            gr.href = parseHyperlink(inst);
        } else if (upper == QLatin1String("PAGE") || upper == QLatin1String("NUMPAGES") ||
                   upper == QLatin1String("SECTIONPAGES")) {
            Run r;
            r.style = currentCharStyle();
            r.field = upper == QLatin1String("PAGE") ? Field::PageNumber : Field::PageCount;
            emitRun(std::move(r));
            gr.dest = Dest::Skip;
        }
        return;
    }
    if (w == "header" || w == "headerr") { beginSink(&m_doc.header); return; }
    if (w == "headerf") { beginSink(&m_doc.headerFirst); return; }
    if (w == "footer" || w == "footerr") { beginSink(&m_doc.footer); return; }
    if (w == "footerf") { beginSink(&m_doc.footerFirst); return; }
    if (w == "footnote") {
        beginSink(&m_doc.notes);
        return;
    }
    if (w == "chftn") {
        Run r;
        r.style = currentCharStyle();
        if (gr.sink == m_body) ++m_footnoteCounter;
        r.text = QString::number(qMax(1, m_footnoteCounter));
        if (gr.sink != m_body) r.text += QLatin1Char(' ');
        emitRun(std::move(r));
        return;
    }
    if (w == "listtext") {
        if (gr.para.ls > 0 && m_overrides.contains(gr.para.ls)) gr.dest = Dest::Skip;
        return;  // otherwise its text is the visible list marker
    }
    if (w == "pntext") return;  // legacy list marker text: keep
    if (w == "upr") { gr.uprSkipChild = true; return; }
    if (w == "ud") return;
    if (skippedDestinations().count(w)) { gr.dest = Dest::Skip; return; }
    if (star) {
        // Unknown \* destination: skip the group cheaply.
        gr.dest = Dest::Skip;
        m_tok.skipGroup();
        m_stack.pop_back();
        return;
    }

    // --- document / section ---------------------------------------------
    if (w == "ansicpg") { if (p > 0) m_docCodepage = p; return; }
    if (w == "ansi") { m_docCodepage = 1252; return; }
    if (w == "mac") { m_docCodepage = 10000; return; }
    if (w == "pc") { m_docCodepage = 437; return; }
    if (w == "pca") { m_docCodepage = 850; return; }
    if (w == "deff") { m_defaultFont = p; return; }
    if (w == "paperw") { m_paperW = p; m_sawPaper = true; return; }
    if (w == "paperh") { m_paperH = p; m_sawPaper = true; return; }
    if (w == "margl") { m_margL = p; return; }
    if (w == "margr") { m_margR = p; return; }
    if (w == "margt") { m_margT = p; return; }
    if (w == "margb") { m_margB = p; return; }
    if (w == "landscape" || w == "lndscpsxn") { if (m_inFirstSection) m_landscape = true; return; }
    if (m_inFirstSection) {
        if (w == "pgwsxn") { m_paperW = p; m_sawPaper = true; return; }
        if (w == "pghsxn") { m_paperH = p; m_sawPaper = true; return; }
        if (w == "marglsxn") { m_margL = p; return; }
        if (w == "margrsxn") { m_margR = p; return; }
        if (w == "margtsxn") { m_margT = p; return; }
        if (w == "margbsxn") { m_margB = p; return; }
        if (w == "headery") { m_headerY = p; return; }
        if (w == "footery") { m_footerY = p; return; }
        if (w == "titlepg") { m_doc.titlePage = true; return; }
    }
    if (w == "cols" && p > 1) { m_doc.report.note(QStringLiteral("Multiple columns shown as one column")); return; }
    if (w == "sectd") { m_sectBreakPage = true; return; }
    if (w == "sbknone" || w == "sbkcol") { m_sectBreakPage = false; return; }
    if (w == "sbkpage" || w == "sbkodd" || w == "sbkeven") { m_sectBreakPage = true; return; }
    if (w == "sect") {
        endParagraph();
        m_inFirstSection = false;
        if (m_sectBreakPage) pageBreak();
        return;
    }
    if (w == "page") {
        if (gr.sink && gr.sink->paraHasContent) {
            endParagraph();
        }
        pageBreak();
        return;
    }
    if (w == "par") { endParagraph(); return; }
    if (w == "line") { text(QString(QChar::LineSeparator)); return; }
    if (w == "tab") { text(QStringLiteral("\t")); return; }

    // --- special characters ---------------------------------------------
    if (w == "u") {
        const ushort u = ushort(p < 0 ? p + 65536 : p);
        text(QString(QChar(u)));
        m_pendingSkip = gr.uc;
        return;
    }
    if (w == "uc") { gr.uc = qBound(0, p, 10); return; }
    static const QHash<QByteArray, ushort> specials = {
        {"emdash", 0x2014}, {"endash", 0x2013}, {"bullet", 0x2022}, {"lquote", 0x2018},
        {"rquote", 0x2019}, {"ldblquote", 0x201C}, {"rdblquote", 0x201D}, {"emspace", 0x2003},
        {"enspace", 0x2002}, {"qmspace", 0x2005}, {"zwj", 0x200D}, {"zwnj", 0x200C},
        {"zwbo", 0x200B}, {"ltrmark", 0x200E}, {"rtlmark", 0x200F},
    };
    if (auto it = specials.constFind(w); it != specials.cend()) {
        text(QString(QChar(*it)));
        return;
    }

    // --- character formatting -------------------------------------------
    CharState &c = gr.chr;
    if (w == "plain") {
        c = CharState{};
        c.font = m_defaultFont;
        return;
    }
    if (w == "f") { c.font = p; return; }
    if (w == "fs") { c.fontSize = qBound(2, t.hasParam ? p : 24, 3276); return; }
    if (w == "b") { c.bold = on; return; }
    if (w == "i") { c.italic = on; return; }
    if (w == "ul" || w == "uld" || w == "uldb" || w == "ulw" || w == "ulth" || w == "uldash" ||
        w == "uldashd" || w == "uldashdd" || w == "ulwave" || w == "ulhwave" || w == "ulldash" ||
        w == "ulthd" || w == "ulthdash") {
        c.underline = on;
        return;
    }
    if (w == "ulnone") { c.underline = false; return; }
    if (w == "strike" || w == "striked") { c.strike = on; return; }
    if (w == "super") { c.vertical = on ? 1 : 0; return; }
    if (w == "sub") { c.vertical = on ? -1 : 0; return; }
    if (w == "nosupersub") { c.vertical = 0; return; }
    if (w == "up") { c.vertical = p > 0 ? 1 : 0; return; }
    if (w == "dn") { c.vertical = p > 0 ? -1 : 0; return; }
    if (w == "caps") { c.caps = on; return; }
    if (w == "scaps") { c.smallCaps = on; return; }
    if (w == "v") { c.hidden = on; return; }
    if (w == "cf") { c.fg = p; return; }
    if (w == "cb" || w == "chcbpat") { c.bg = p; return; }
    if (w == "highlight") { c.highlight = p; return; }
    if (w == "expndtw") { c.letterSpacing = p * kTwip; return; }
    if (w == "expnd") { c.letterSpacing = p / 4.0; return; }

    // --- paragraph formatting -------------------------------------------
    ParaState &ps = gr.para;
    if (w == "pard") {
        ps = ParaState{};
        m_border.side = BorderTarget::None;
        return;
    }
    if (w == "ql") { ps.style.align = Qt::AlignLeft; return; }
    if (w == "qr") { ps.style.align = Qt::AlignRight; return; }
    if (w == "qc") { ps.style.align = Qt::AlignHCenter; return; }
    if (w == "qj" || w == "qd") { ps.style.align = Qt::AlignJustify; return; }
    if (w == "li" || w == "lin") { ps.style.leftIndent = p * kTwip; return; }
    if (w == "ri" || w == "rin") { ps.style.rightIndent = p * kTwip; return; }
    if (w == "fi") { ps.style.firstLineIndent = p * kTwip; return; }
    if (w == "sb") { ps.style.spaceBefore = qMax(0, p) * kTwip; return; }
    if (w == "sa") { ps.style.spaceAfter = qMax(0, p) * kTwip; return; }
    if (w == "sl") {
        if (p == 0) {
            ps.style.lineRule = ParaStyle::LineRule::Auto;
        } else if (p < 0) {
            ps.style.lineRule = ParaStyle::LineRule::Exact;
            ps.style.lineValue = -p * kTwip;
        } else {
            ps.style.lineRule = ParaStyle::LineRule::AtLeast;
            ps.style.lineValue = p * kTwip;
        }
        return;
    }
    if (w == "slmult") {
        if (p == 1 && ps.style.lineRule == ParaStyle::LineRule::AtLeast) {
            ps.style.lineRule = ParaStyle::LineRule::Proportional;
            ps.style.lineValue = ps.style.lineValue / 12.0 * 100.0;  // 240 twips = single
        }
        return;
    }
    if (w == "keepn") { ps.style.keepWithNext = true; return; }
    if (w == "pagebb") { ps.style.pageBreakBefore = true; return; }
    if (w == "tqr") { ps.nextTabType = TabStop::Right; return; }
    if (w == "tqc") { ps.nextTabType = TabStop::Center; return; }
    if (w == "tqdec") { ps.nextTabType = TabStop::Decimal; return; }
    if (w == "tx" || w == "tb") {
        if (ps.style.tabs.size() < Limits::kMaxTabStops) ps.style.tabs << TabStop{p * kTwip, ps.nextTabType};
        ps.nextTabType = TabStop::Left;
        return;
    }
    if (w == "intbl") { ps.inTable = true; return; }
    if (w == "itap") {
        ps.inTable = p > 0;
        if (p > 1) m_doc.report.count(QStringLiteral("Nested tables flattened"));
        return;
    }
    if (w == "ls") { ps.ls = p; return; }
    if (w == "ilvl") { ps.ilvl = p; return; }
    if (w == "s") { ps.styleIndex = p; return; }
    if (w == "outlinelevel") { ps.style.headingLevel = qBound(0, p, 8) + 1; return; }
    if (w == "cbpat") { if (p > 0 && p < m_colors.size()) ps.style.background = m_colors[p]; return; }
    if (w == "brdrt" || w == "brdrb" || w == "brdrl" || w == "brdrr" || w == "box") {
        m_border.side = BorderTarget::Para;
        m_doc.report.count(QStringLiteral("Paragraph borders not drawn"));
        return;
    }

    // --- tables -----------------------------------------------------------
    if (w == "trowd") {
        m_rowDef = RowDef{};
        m_border.side = BorderTarget::None;
        return;
    }
    if (w == "trleft") { m_rowDef.left = clampTwips(p); return; }
    if (w == "trqc") { m_rowDef.align = Qt::AlignHCenter; return; }
    if (w == "trqr") { m_rowDef.align = Qt::AlignRight; return; }
    if (w == "trhdr") { m_rowDef.header = true; return; }
    if (w == "trrh") { m_rowDef.height = qAbs(p) * kTwip; return; }
    if (w == "clbrdrt") { m_border.side = BorderTarget::CellTop; return; }
    if (w == "clbrdrl") { m_border.side = BorderTarget::CellLeft; return; }
    if (w == "clbrdrb") { m_border.side = BorderTarget::CellBottom; return; }
    if (w == "clbrdrr") { m_border.side = BorderTarget::CellRight; return; }
    auto border = [&]() -> Border * {
        switch (m_border.side) {
        case BorderTarget::CellTop: return &m_rowDef.pending.top;
        case BorderTarget::CellLeft: return &m_rowDef.pending.left;
        case BorderTarget::CellBottom: return &m_rowDef.pending.bottom;
        case BorderTarget::CellRight: return &m_rowDef.pending.right_;
        default: return nullptr;
        }
    };
    if (w.startsWith("brdr")) {
        if (Border *b = border()) {
            if (w == "brdrnil" || w == "brdrnone") b->width = 0;
            else if (w == "brdrw") b->width = qBound(0.25, p * kTwip, 6.0);
            else if (w == "brdrcf") {
                if (p > 0 && p < m_colors.size() && m_colors[p].isValid()) b->color = m_colors[p];
            } else if (b->width == 0) b->width = 0.5;  // any style word turns it on
        }
        return;
    }
    if (w == "clcbpat") { m_rowDef.pending.shading = p; return; }
    if (w == "clmgf") { m_rowDef.pending.hMergeFirst = true; return; }
    if (w == "clmrg") { m_rowDef.pending.hMerge = true; return; }
    if (w == "clvmgf") { m_rowDef.pending.vMergeFirst = true; return; }
    if (w == "clvmrg") { m_rowDef.pending.vMerge = true; return; }
    if (w == "clvertalc") { m_rowDef.pending.vAlign = Qt::AlignVCenter; return; }
    if (w == "clvertalb") { m_rowDef.pending.vAlign = Qt::AlignBottom; return; }
    if (w == "cellx") {
        m_rowDef.pending.right = clampTwips(p);
        if (m_rowDef.cells.size() < Limits::kMaxTableColumns) m_rowDef.cells << m_rowDef.pending;
        m_rowDef.pending = CellDef{};
        m_border.side = BorderTarget::None;
        return;
    }
    if (w == "cell" || w == "nestcell") {
        if (w == "nestcell") { endParagraph(); return; }
        endCell();
        return;
    }
    if (w == "row") { endRow(); return; }
    if (w == "nestrow") return;
    // Unknown control words are ignored.
}

}  // namespace

flow::Document import(const QByteArray &data)
{
    Interpreter in(data);
    return in.run();
}

}  // namespace rtf
