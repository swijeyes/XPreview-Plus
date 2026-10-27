// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "OdfStyles.h"

#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QRegularExpression>

namespace odf {

QString canonicalName(QStringView nsUri, QStringView local)
{
    static const QHash<QString, QString> prefixes = {
        {ns::office, QStringLiteral("office")}, {ns::style, QStringLiteral("style")},
        {ns::text, QStringLiteral("text")},     {ns::table, QStringLiteral("table")},
        {ns::draw, QStringLiteral("draw")},     {ns::fo, QStringLiteral("fo")},
        {ns::svg, QStringLiteral("svg")},       {ns::xlink, QStringLiteral("xlink")},
    };
    const QString p = prefixes.value(nsUri.toString(), QStringLiteral("x"));
    return p + QLatin1Char(':') + local;
}

double toPoints(QStringView v, bool *ok)
{
    v = v.trimmed();
    static const QRegularExpression re(QStringLiteral(R"(^(-?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\s*([a-z%]*)$)"));
    const auto m = re.matchView(v);
    if (!m.hasMatch()) {
        if (ok) *ok = false;
        return 0;
    }
    // Clamp at the source: no length from a file may be non-finite or huge.
    const double n = SafeImage::clampLength(m.captured(1).toDouble());
    const QString unit = m.captured(2);
    if (ok) *ok = unit != QLatin1String("%");
    if (unit == QLatin1String("%")) return n;
    if (unit == QLatin1String("cm")) return n * 72.0 / 2.54;
    if (unit == QLatin1String("mm")) return n * 72.0 / 25.4;
    if (unit == QLatin1String("in") || unit == QLatin1String("inch")) return n * 72.0;
    if (unit == QLatin1String("pc")) return n * 12.0;
    if (unit == QLatin1String("px")) return n * 0.75;
    if (unit == QLatin1String("twip")) return n / 20.0;
    return n;  // pt or unitless
}

QColor toColor(QStringView v)
{
    if (v.isEmpty() || v == u"transparent") return {};
    QColor c = QColor::fromString(v);
    return c.isValid() ? c : QColor();
}

flow::ListLevel ListStyle::level(int i) const
{
    if (i >= 0 && i < levels.size()) return levels[i];
    if (!levels.isEmpty()) return levels.last();
    flow::ListLevel l;
    l.marker = flow::ListMarker::Disc;
    return l;
}

void Styles::readProps(QXmlStreamReader &r, Props &into)
{
    for (const QXmlStreamAttribute &a : r.attributes())
        into.insert(canonicalName(a.namespaceUri(), a.name()), a.value().toString());
}

void Styles::readFontFaces(QXmlStreamReader &r)
{
    while (r.readNextStartElement()) {
        if (r.name() == u"font-face") {
            QString family = r.attributes().value(ns::svg, u"font-family").toString();
            family.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
            const QString name = r.attributes().value(ns::style, u"name").toString();
            if (family.isEmpty()) family = name;
            m_fontFaces.insert(name, family.section(QLatin1Char(','), 0, 0).trimmed());
        }
        r.skipCurrentElement();
    }
}

void Styles::readStyles(QXmlStreamReader &r)
{
    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (r.namespaceUri() == ns::style && name == u"style") readStyle(r, false);
        else if (r.namespaceUri() == ns::style && name == u"default-style") readStyle(r, true);
        else if (r.namespaceUri() == ns::text && name == u"list-style") readListStyle(r);
        else if (r.namespaceUri() == ns::text && name == u"outline-style") readOutlineStyle(r);
        else if (r.namespaceUri() == ns::style && name == u"page-layout") readPageLayout(r);
        else r.skipCurrentElement();
    }
}

void Styles::readStyle(QXmlStreamReader &r, bool isDefault)
{
    Style s;
    const auto a = r.attributes();
    s.name = a.value(ns::style, u"name").toString();
    s.family = a.value(ns::style, u"family").toString();
    s.parent = a.value(ns::style, u"parent-style-name").toString();
    s.listStyle = a.value(ns::style, u"list-style-name").toString();
    s.masterPage = a.value(ns::style, u"master-page-name").toString();
    s.outlineLevel = a.value(ns::style, u"default-outline-level").toInt();
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == u"text-properties") readProps(r, s.text);
        else if (n == u"paragraph-properties") {
            readProps(r, s.para);
            // tab stops are child elements
            while (r.readNextStartElement()) {
                if (r.name() == u"tab-stops") {
                    s.hasTabs = true;
                    while (r.readNextStartElement()) {
                        if (r.name() == u"tab-stop") {
                            flow::TabStop t;
                            t.position = toPoints(r.attributes().value(ns::style, u"position"));
                            const auto type = r.attributes().value(ns::style, u"type");
                            if (type == u"right") t.type = flow::TabStop::Right;
                            else if (type == u"center") t.type = flow::TabStop::Center;
                            else if (type == u"char") t.type = flow::TabStop::Decimal;
                            if (s.tabs.size() < 64) s.tabs << t;
                        }
                        r.skipCurrentElement();
                    }
                } else {
                    r.skipCurrentElement();
                }
            }
            continue;
        }
        else if (n == u"table-properties") readProps(r, s.table);
        else if (n == u"table-column-properties") readProps(r, s.column);
        else if (n == u"table-row-properties") readProps(r, s.row);
        else if (n == u"table-cell-properties") readProps(r, s.cell);
        else if (n == u"graphic-properties") readProps(r, s.graphic);
        else if (n == u"section-properties") readProps(r, s.section);
        if (r.isStartElement()) r.skipCurrentElement();
    }
    if (isDefault) m_defaults.insert(s.family, s);
    else if (!s.name.isEmpty()) m_styles.insert(s.family + QLatin1Char('/') + s.name, s);
}

static flow::ListMarker markerForFormat(QStringView f)
{
    if (f == u"1") return flow::ListMarker::Decimal;
    if (f == u"a") return flow::ListMarker::LowerAlpha;
    if (f == u"A") return flow::ListMarker::UpperAlpha;
    if (f == u"i") return flow::ListMarker::LowerRoman;
    if (f == u"I") return flow::ListMarker::UpperRoman;
    return flow::ListMarker::None;
}

static double readLevelIndent(QXmlStreamReader &r)
{
    double indent = 0;
    while (r.readNextStartElement()) {
        if (r.name() == u"list-level-properties") {
            const auto a = r.attributes();
            const double before = toPoints(a.value(ns::text, u"space-before"));
            const double label = toPoints(a.value(ns::text, u"min-label-width"));
            indent = before + label;
            while (r.readNextStartElement()) {
                if (r.name() == u"list-level-label-alignment") {
                    bool ok = false;
                    const double ml = toPoints(r.attributes().value(ns::fo, u"margin-left"), &ok);
                    if (ok) indent = ml;
                }
                r.skipCurrentElement();
            }
        } else {
            r.skipCurrentElement();
        }
    }
    return indent;
}

void Styles::readListStyle(QXmlStreamReader &r)
{
    ListStyle ls;
    const QString name = r.attributes().value(ns::style, u"name").toString();
    while (r.readNextStartElement()) {
        const auto n = r.name();
        const auto a = r.attributes();
        const int level = qBound(1, a.value(ns::text, u"level").toInt(), 10);
        flow::ListLevel l;
        if (n == u"list-level-style-bullet") {
            l.marker = flow::ListMarker::Text;
            l.bulletText = a.value(ns::text, u"bullet-char").toString();
            l.suffix.clear();
        } else if (n == u"list-level-style-number") {
            l.marker = markerForFormat(a.value(ns::style, u"num-format"));
            l.prefix = a.value(ns::style, u"num-prefix").toString();
            l.suffix = a.value(ns::style, u"num-suffix").toString();
            l.start = a.hasAttribute(ns::text, u"start-value") ? a.value(ns::text, u"start-value").toInt() : 1;
        } else if (n == u"list-level-style-image") {
            l.marker = flow::ListMarker::Disc;
        } else {
            r.skipCurrentElement();
            continue;
        }
        l.indent = readLevelIndent(r);
        while (ls.levels.size() < level) ls.levels << flow::ListLevel{};
        ls.levels[level - 1] = l;
    }
    m_lists.insert(name, ls);
}

void Styles::readOutlineStyle(QXmlStreamReader &r)
{
    m_outline.clear();
    while (r.readNextStartElement()) {
        if (r.name() == u"outline-level-style") {
            const auto a = r.attributes();
            const int level = qBound(1, a.value(ns::text, u"level").toInt(), 10);
            OutlineLevel o;
            o.format = a.value(ns::style, u"num-format").toString();
            o.prefix = a.value(ns::style, u"num-prefix").toString();
            o.suffix = a.value(ns::style, u"num-suffix").toString();
            o.displayLevels = qMax(1, a.value(ns::text, u"display-levels").toInt());
            o.start = a.hasAttribute(ns::text, u"start-value") ? a.value(ns::text, u"start-value").toInt() : 1;
            while (m_outline.size() < level) m_outline << OutlineLevel{};
            m_outline[level - 1] = o;
        }
        r.skipCurrentElement();
    }
}

void Styles::readPageLayout(QXmlStreamReader &r)
{
    PageLayout pl;
    const QString name = r.attributes().value(ns::style, u"name").toString();
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == u"page-layout-properties") {
            readProps(r, pl.page);
            r.skipCurrentElement();
        } else if (n == u"header-style" || n == u"footer-style") {
            Props &target = n == u"header-style" ? pl.header : pl.footer;
            while (r.readNextStartElement()) {
                if (r.name() == u"header-footer-properties") readProps(r, target);
                r.skipCurrentElement();
            }
        } else {
            r.skipCurrentElement();
        }
    }
    m_layouts.insert(name, pl);
}

const Style *Styles::find(const QString &family, const QString &name) const
{
    auto it = m_styles.constFind(family + QLatin1Char('/') + name);
    return it == m_styles.cend() ? nullptr : &*it;
}

const ListStyle *Styles::listStyle(const QString &name) const
{
    auto it = m_lists.constFind(name);
    return it == m_lists.cend() ? nullptr : &*it;
}

const PageLayout *Styles::pageLayout(const QString &name) const
{
    auto it = m_layouts.constFind(name);
    return it == m_layouts.cend() ? nullptr : &*it;
}

void Styles::mergeText(Props &into, const Props &over)
{
    for (auto it = over.cbegin(); it != over.cend(); ++it) {
        if (it.key() == QLatin1String("fo:font-size") && it.value().endsWith(QLatin1Char('%'))) {
            const double base = toPoints(into.value(QStringLiteral("fo:font-size"), QStringLiteral("12pt")));
            const double pct = QStringView(it.value()).chopped(1).toDouble();
            into.insert(it.key(), QString::number(base * pct / 100.0) + QStringLiteral("pt"));
        } else {
            into.insert(it.key(), it.value());
        }
    }
}

Style Styles::resolve(const QString &family, const QString &name) const
{
    // Collect the chain child -> root, then merge root first.
    QList<const Style *> chain;
    const Style *s = find(family, name);
    int guard = 0;
    while (s && guard++ < 32) {
        chain.prepend(s);
        s = s->parent.isEmpty() ? nullptr : find(family, s->parent);
    }
    Style out;
    auto def = m_defaults.constFind(family);
    if (def != m_defaults.cend()) chain.prepend(&*def);
    for (const Style *st : std::as_const(chain)) {
        mergeText(out.text, st->text);
        for (auto it = st->para.cbegin(); it != st->para.cend(); ++it) out.para.insert(it.key(), it.value());
        for (auto it = st->table.cbegin(); it != st->table.cend(); ++it) out.table.insert(it.key(), it.value());
        for (auto it = st->column.cbegin(); it != st->column.cend(); ++it) out.column.insert(it.key(), it.value());
        for (auto it = st->row.cbegin(); it != st->row.cend(); ++it) out.row.insert(it.key(), it.value());
        for (auto it = st->cell.cbegin(); it != st->cell.cend(); ++it) out.cell.insert(it.key(), it.value());
        for (auto it = st->graphic.cbegin(); it != st->graphic.cend(); ++it) out.graphic.insert(it.key(), it.value());
        for (auto it = st->section.cbegin(); it != st->section.cend(); ++it) out.section.insert(it.key(), it.value());
        if (!st->listStyle.isEmpty()) out.listStyle = st->listStyle;
        if (st->outlineLevel) out.outlineLevel = st->outlineLevel;
        if (st->hasTabs) {
            out.tabs = st->tabs;
            out.hasTabs = true;
        }
    }
    if (!chain.isEmpty()) {
        out.name = name;
        out.family = family;
        // Master page applies only from the named style itself.
        if (const Style *own = find(family, name)) out.masterPage = own->masterPage;
    }
    return out;
}

ResolvedPara Styles::resolveParagraph(const QString &name) const
{
    const Style s = resolve(QStringLiteral("paragraph"), name);
    ResolvedPara r;
    r.para = toParaStyle(s.para, &s);
    r.textProps = s.text;
    r.listStyle = s.listStyle;
    r.masterPage = s.masterPage;
    r.outlineLevel = s.outlineLevel;
    return r;
}

Props Styles::resolveText(const QString &name, const Props &base) const
{
    Props out = base;
    QList<const Style *> chain;
    const Style *s = find(QStringLiteral("text"), name);
    int guard = 0;
    while (s && guard++ < 32) {
        chain.prepend(s);
        s = s->parent.isEmpty() ? nullptr : find(QStringLiteral("text"), s->parent);
    }
    for (const Style *st : std::as_const(chain)) mergeText(out, st->text);
    return out;
}

QString Styles::fontFamily(const Props &t) const
{
    const QString face = t.value(QStringLiteral("style:font-name"));
    if (!face.isEmpty()) return m_fontFaces.value(face, face);
    QString fam = t.value(QStringLiteral("fo:font-family"));
    fam.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    return fam.section(QLatin1Char(','), 0, 0).trimmed();
}

flow::CharStyle Styles::toCharStyle(const Props &t) const
{
    flow::CharStyle c;
    c.family = fontFamily(t);
    if (t.contains(QStringLiteral("fo:font-size"))) c.size = toPoints(t.value(QStringLiteral("fo:font-size")));
    const QString weight = t.value(QStringLiteral("fo:font-weight"));
    c.bold = weight == QLatin1String("bold") || weight.toInt() >= 600;
    const QString style = t.value(QStringLiteral("fo:font-style"));
    c.italic = style == QLatin1String("italic") || style == QLatin1String("oblique");
    const QString ul = t.value(QStringLiteral("style:text-underline-style"));
    c.underline = !ul.isEmpty() && ul != QLatin1String("none");
    const QString lt = t.value(QStringLiteral("style:text-line-through-style"));
    c.strike = !lt.isEmpty() && lt != QLatin1String("none");
    c.smallCaps = t.value(QStringLiteral("fo:font-variant")) == QLatin1String("small-caps");
    c.allCaps = t.value(QStringLiteral("fo:text-transform")) == QLatin1String("uppercase");
    const QString pos = t.value(QStringLiteral("style:text-position"));
    if (pos.startsWith(QLatin1String("super"))) c.vertical = 1;
    else if (pos.startsWith(QLatin1String("sub"))) c.vertical = -1;
    else if (!pos.isEmpty()) {
        QString first = pos.section(QLatin1Char(' '), 0, 0).trimmed();
        if (first.endsWith(QLatin1Char('%'))) first.chop(1);
        const double v = first.toDouble();
        c.vertical = v > 0 ? 1 : (v < 0 ? -1 : 0);
    }
    const QString color = t.value(QStringLiteral("fo:color"));
    if (!color.isEmpty() && t.value(QStringLiteral("style:use-window-font-color")) != QLatin1String("true"))
        c.color = toColor(color);
    c.background = toColor(t.value(QStringLiteral("fo:background-color")));
    bool ok = false;
    const double ls = toPoints(t.value(QStringLiteral("fo:letter-spacing")), &ok);
    if (ok) c.letterSpacing = ls;
    c.hidden = t.value(QStringLiteral("text:display")) == QLatin1String("none");
    return c;
}

flow::ParaStyle Styles::toParaStyle(const Props &p, const Style *tabsFrom)
{
    flow::ParaStyle s;
    const QString align = p.value(QStringLiteral("fo:text-align"));
    if (align == QLatin1String("center")) s.align = Qt::AlignHCenter;
    else if (align == QLatin1String("end") || align == QLatin1String("right")) s.align = Qt::AlignRight;
    else if (align == QLatin1String("justify")) s.align = Qt::AlignJustify;
    s.leftIndent = toPoints(p.value(QStringLiteral("fo:margin-left")));
    s.rightIndent = toPoints(p.value(QStringLiteral("fo:margin-right")));
    s.firstLineIndent = toPoints(p.value(QStringLiteral("fo:text-indent")));
    s.spaceBefore = qMax(0.0, toPoints(p.value(QStringLiteral("fo:margin-top"))));
    s.spaceAfter = qMax(0.0, toPoints(p.value(QStringLiteral("fo:margin-bottom"))));
    const QString lh = p.value(QStringLiteral("fo:line-height"));
    if (lh.endsWith(QLatin1Char('%'))) {
        s.lineRule = flow::ParaStyle::LineRule::Proportional;
        s.lineValue = QStringView(lh).chopped(1).toDouble();
    } else if (!lh.isEmpty() && lh != QLatin1String("normal")) {
        bool ok = false;
        const double v = toPoints(lh, &ok);
        if (ok && v > 0) {
            s.lineRule = flow::ParaStyle::LineRule::Exact;
            s.lineValue = v;
        }
    }
    const QString least = p.value(QStringLiteral("style:line-height-at-least"));
    if (!least.isEmpty()) {
        s.lineRule = flow::ParaStyle::LineRule::AtLeast;
        s.lineValue = toPoints(least);
    }
    s.pageBreakBefore = p.value(QStringLiteral("fo:break-before")) == QLatin1String("page");
    s.pageBreakAfter = p.value(QStringLiteral("fo:break-after")) == QLatin1String("page");
    s.keepWithNext = p.value(QStringLiteral("fo:keep-with-next")) == QLatin1String("always");
    s.background = toColor(p.value(QStringLiteral("fo:background-color")));
    if (tabsFrom && tabsFrom->hasTabs) {
        // ODF tab positions are relative to the paragraph's left margin
        // (LibreOffice default); Qt's are relative to the block too.
        s.tabs = tabsFrom->tabs;
    }
    return s;
}

}  // namespace odf
