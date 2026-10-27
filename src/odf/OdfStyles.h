// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "flow/FlowModel.h"

#include <QHash>
#include <QString>
#include <QXmlStreamReader>

namespace odf {

// Namespace URIs (prefixes are not reliable; resolve by URI).
namespace ns {
inline const QString office = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
inline const QString style = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
inline const QString text = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
inline const QString table = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
inline const QString draw = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
inline const QString fo = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
inline const QString svg = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
inline const QString xlink = QStringLiteral("http://www.w3.org/1999/xlink");
inline const QString dc = QStringLiteral("http://purl.org/dc/elements/1.1/");
inline const QString meta = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:meta:1.0");
}  // namespace ns

// "fo:margin-left" style keys, from any namespace URI.
QString canonicalName(QStringView nsUri, QStringView local);
// Lengths: cm, mm, in, pt, pc, px; returns points. `ok` false if not a length.
double toPoints(QStringView value, bool *ok = nullptr);
QColor toColor(QStringView value);

using Props = QHash<QString, QString>;

struct Style {
    QString name, family, parent, listStyle, masterPage;
    int outlineLevel = 0;
    Props text, para, table, column, row, cell, graphic, section;
    QList<flow::TabStop> tabs;
    bool hasTabs = false;
};

struct ListStyle {
    QList<flow::ListLevel> levels;  // index = level - 1
    flow::ListLevel level(int i) const;
};

struct OutlineLevel {
    QString format, prefix, suffix;
    int displayLevels = 1;
    int start = 1;
};

struct PageLayout {
    Props page, header, footer;
};

struct ResolvedPara {
    flow::ParaStyle para;
    Props textProps;  // merged character properties
    QString listStyle;
    QString masterPage;
    int outlineLevel = 0;
};

class Styles {
public:
    // Reads children of office:font-face-decls / office:styles /
    // office:automatic-styles until the matching end element.
    void readFontFaces(QXmlStreamReader &r);
    void readStyles(QXmlStreamReader &r);

    const Style *find(const QString &family, const QString &name) const;
    const ListStyle *listStyle(const QString &name) const;
    const PageLayout *pageLayout(const QString &name) const;
    const QList<OutlineLevel> &outline() const { return m_outline; }

    // Merged properties along the parent chain, default style last.
    ResolvedPara resolveParagraph(const QString &name) const;
    Props resolveText(const QString &name, const Props &base) const;
    Style resolve(const QString &family, const QString &name) const;

    flow::CharStyle toCharStyle(const Props &text) const;
    QString fontFamily(const Props &text) const;
    static flow::ParaStyle toParaStyle(const Props &para, const Style *tabsFrom);

private:
    void readStyle(QXmlStreamReader &r, bool isDefault);
    void readListStyle(QXmlStreamReader &r);
    void readOutlineStyle(QXmlStreamReader &r);
    void readPageLayout(QXmlStreamReader &r);
    static void readProps(QXmlStreamReader &r, Props &into);
    static void mergeText(Props &into, const Props &over);

    QHash<QString, Style> m_styles;          // key: family + '/' + name
    QHash<QString, Style> m_defaults;        // key: family
    QHash<QString, ListStyle> m_lists;
    QHash<QString, PageLayout> m_layouts;
    QHash<QString, QString> m_fontFaces;     // face name -> family
    QList<OutlineLevel> m_outline;
};

}  // namespace odf
