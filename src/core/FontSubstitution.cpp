// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "FontSubstitution.h"

#include "FidelityReport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSet>

// Resources compiled into a static library must be initialised explicitly,
// or the linker drops them.
static void initResources() { Q_INIT_RESOURCE(resources); }

namespace {

struct Table {
    QHash<QString, QStringList> candidates;  // lower-case key
    QHash<QString, QString> equivalents;     // lower-case key -> same-metric family
    QSet<QString> families;                  // lower-case installed families
    QHash<QString, QString> cache;
    QMutex lock;
};

Table &table()
{
    static Table *t = [] {
        initResources();
        auto *t = new Table;
        QFile f(QStringLiteral(":/font_substitutions.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject eq = obj.value(QStringLiteral("_equivalents")).toObject();
            for (auto it = eq.begin(); it != eq.end(); ++it)
                if (it.value().isString()) t->equivalents.insert(it.key().toLower(), it.value().toString());
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (!it.value().isArray()) continue;
                QStringList list;
                for (const QJsonValue &v : it.value().toArray()) list << v.toString();
                t->candidates.insert(it.key().toLower(), list);
            }
        }
        for (const QString &fam : QFontDatabase::families()) t->families.insert(fam.toLower());
        return t;
    }();
    return *t;
}

QStringList fontDirs()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs = {app + QStringLiteral("/../Resources/fonts"), app + QStringLiteral("/fonts"),
                        app + QStringLiteral("/../share/xpreview/fonts")};
#ifdef XPREVIEW_DEV_FONT_DIR
    // Only for unbundled developer builds: an installed app never loads
    // fonts from outside its own bundle/install prefix.
    bool bundled = false;
    for (const QString &d : dirs) bundled = bundled || QDir(d).exists();
    if (!bundled) dirs << QStringLiteral(XPREVIEW_DEV_FONT_DIR);
#endif
    return dirs;
}

// "Times-Bold", "ArialMT", "TimesNewRomanPSMT" -> "Times", "Arial",
// "Times New Roman". RTF from Apple apps and PDFs use PostScript names.
QString familyFromPostScript(const QString &name)
{
    QString base = name.section(QLatin1Char('-'), 0, 0);
    for (const char *suffix : {"PSMT", "MT", "PS"}) {
        if (base.endsWith(QLatin1String(suffix)) && base.size() > int(qstrlen(suffix)) + 2) {
            base.chop(int(qstrlen(suffix)));
            break;
        }
    }
    if (!base.contains(QLatin1Char(' '))) {
        QString spaced;
        for (int i = 0; i < base.size(); ++i) {
            if (i > 0 && base[i].isUpper() && base[i - 1].isLower()) spaced += QLatin1Char(' ');
            spaced += base[i];
        }
        base = spaced;
    }
    return base;
}

}  // namespace

namespace FontSubstitution {

void loadBundledFonts()
{
    for (const QString &dir : fontDirs()) {
        QDir d(dir);
        for (const QString &file :
             d.entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files))
            QFontDatabase::addApplicationFont(d.filePath(file));
    }
    Table &t = table();
    QMutexLocker l(&t.lock);
    for (const QString &fam : QFontDatabase::families()) t.families.insert(fam.toLower());
}

QString resolve(const QString &family, FidelityReport *report)
{
    if (family.isEmpty()) return family;
    Table &t = table();
    QMutexLocker l(&t.lock);
    const QString key = family.toLower();
    auto cached = t.cache.constFind(key);
    // Same-design equivalents are swapped in silently when available.
    const QString equivalent = t.equivalents.value(key);
    if (!equivalent.isEmpty() && t.families.contains(equivalent.toLower())) return equivalent;
    QString result;
    if (cached != t.cache.cend()) {
        result = *cached;
    } else {
        result = family;
        QString lookup = key;
        if (!t.families.contains(key)) {
            const QString ps = familyFromPostScript(family);
            if (t.families.contains(ps.toLower())) {
                result = ps;
            } else {
                lookup = ps.toLower();
            }
        }
        if (!t.families.contains(result.toLower())) {
            for (const QString &c : t.candidates.value(lookup, t.candidates.value(key))) {
                if (t.families.contains(c.toLower())) {
                    result = c;
                    break;
                }
            }
        }
        t.cache.insert(key, result);
    }
    // PostScript-name normalisation ("Times-Bold" -> "Times") is not a
    // substitution worth reporting.
    if (report && result.compare(family, Qt::CaseInsensitive) != 0 &&
        result.compare(familyFromPostScript(family), Qt::CaseInsensitive) != 0)
        report->fontSubstituted(familyFromPostScript(family), result);
    // Unknown and unmapped: Qt's own fallback picks something sensible; we
    // still note it so the user knows layout may differ.
    if (report && result == family && !t.families.contains(key))
        report->fontSubstituted(familyFromPostScript(family), QStringLiteral("system default"));
    return result;
}

bool isSymbolFont(const QString &family)
{
    const QString f = family.toLower();
    return f == QLatin1String("symbol") || f.startsWith(QLatin1String("wingdings")) ||
           f == QLatin1String("opensymbol");
}

QChar mapSymbolChar(const QString &family, QChar c)
{
    // Private-use range F0xx is how Symbol-font glyphs are often encoded.
    ushort u = c.unicode();
    if (u >= 0xF000 && u <= 0xF0FF) u -= 0xF000;
    const QString f = family.toLower();
    if (f == QLatin1String("symbol")) {
        switch (u) {
        case 0xB7: return QChar(0x2022);  // bullet
        case 0xA7: return QChar(0x25AA);
        case 0xAE: return QChar(0x2192);
        case 0xD8: return QChar(0x00AC);
        case 0x6F: return QChar(0x25E6);
        }
    } else if (f.startsWith(QLatin1String("wingdings"))) {
        switch (u) {
        case 0x6C: return QChar(0x25CF);
        case 0x6E: return QChar(0x25A0);
        case 0x71: return QChar(0x274F);
        case 0xA7: return QChar(0x25AA);
        case 0xA8: return QChar(0x25FB);
        case 0xD8: return QChar(0x27A2);
        case 0xFC: return QChar(0x2714);
        case 0xFB: return QChar(0x2718);
        case 0x76: return QChar(0x2756);
        case 0x77: return QChar(0x2B25);
        case 0xE0: return QChar(0x2192);
        }
    }
    return u < 0x20 ? c : (c.unicode() >= 0xF000 && c.unicode() <= 0xF0FF ? QChar(0x2022) : c);
}

}  // namespace FontSubstitution
