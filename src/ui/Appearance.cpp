// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "Appearance.h"

#include "AppStyle.h"

#include <QApplication>
#include <QSettings>
#include <QStyleHints>
#include <QWidget>

namespace {
const QString kKindKey = QStringLiteral("appearance/kind");
const QString kColorKey = QStringLiteral("appearance/color");
const QString kModeKey = QStringLiteral("appearance/mode");
const QString kLegacyThemeKey = QStringLiteral("appearance/theme");

// A full flat palette, built from one background, surface, text and accent so
// every widget follows the Modern look. Values mirror XTunes+'s flat theme.
QPalette flatPalette(bool dark)
{
    const QColor window = dark ? QColor(0x1c, 0x1c, 0x1e) : QColor(0xf4, 0xf5, 0xf7);
    const QColor base = dark ? QColor(0x1c, 0x1c, 0x1e) : QColor(0xff, 0xff, 0xff);
    const QColor alt = dark ? QColor(0x24, 0x24, 0x26) : QColor(0xf4, 0xf6, 0xfb);
    const QColor text = dark ? QColor(0xe5, 0xe5, 0xea) : QColor(0x1d, 0x1d, 0x1f);
    const QColor button = dark ? QColor(0x3a, 0x3a, 0x3c) : QColor(0xff, 0xff, 0xff);
    const QColor tipBg = dark ? QColor(0x2a, 0x2a, 0x2c) : QColor(0xff, 0xff, 0xff);
    const QColor dim = dark ? QColor(0x8e, 0x8e, 0x93) : QColor(0x86, 0x86, 0x8b);
    const QColor disabled = dark ? QColor(0x6b, 0x6b, 0x70) : QColor(0xb0, 0xb0, 0xb5);
    const QColor link = dark ? QColor(0x6a, 0xa3, 0xf0) : QColor(0x0a, 0x84, 0xff);
    const QColor accent(0x0a, 0x84, 0xff);
    const QColor border = dark ? QColor(0x0c, 0x0c, 0x0d) : QColor(0xc6, 0xc6, 0xcc);

    QPalette pal;
    for (auto g : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(g, QPalette::Window, window);
        pal.setColor(g, QPalette::WindowText, text);
        pal.setColor(g, QPalette::Base, base);
        pal.setColor(g, QPalette::AlternateBase, alt);
        pal.setColor(g, QPalette::ToolTipBase, tipBg);
        pal.setColor(g, QPalette::ToolTipText, text);
        pal.setColor(g, QPalette::Text, text);
        pal.setColor(g, QPalette::Button, button);
        pal.setColor(g, QPalette::ButtonText, text);
        pal.setColor(g, QPalette::BrightText, Qt::white);
        pal.setColor(g, QPalette::PlaceholderText, dim);
        pal.setColor(g, QPalette::Link, link);
        pal.setColor(g, QPalette::LinkVisited, link);
        pal.setColor(g, QPalette::Light, dark ? QColor(0x48, 0x48, 0x4a) : QColor(0xff, 0xff, 0xff));
        pal.setColor(g, QPalette::Midlight, dark ? QColor(0x3a, 0x3a, 0x3c) : QColor(0xf0, 0xf0, 0xf3));
        pal.setColor(g, QPalette::Mid, border);
        pal.setColor(g, QPalette::Dark, dark ? QColor(0x00, 0x00, 0x00) : QColor(0xa8, 0xa8, 0xae));
        pal.setColor(g, QPalette::Shadow, dark ? QColor(0x00, 0x00, 0x00) : QColor(0x8c, 0x8c, 0x92));
    }
    // Inactive (background window) selection reads muted on both looks.
    pal.setColor(QPalette::Active, QPalette::Highlight, accent);
    pal.setColor(QPalette::Inactive, QPalette::Highlight, dark ? QColor(0x3a, 0x3a, 0x3c) : QColor(0xd4, 0xd4, 0xda));
    for (auto g : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(g, QPalette::Accent, accent);
        pal.setColor(g, QPalette::HighlightedText,
                     g == QPalette::Active ? QColor(Qt::white) : text);
    }
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, disabled);
    pal.setColor(QPalette::Disabled, QPalette::Base, window);
    return pal;
}
}  // namespace

Appearance *Appearance::instance()
{
    static Appearance *a = new Appearance;  // lives until exit, like qApp
    return a;
}

Appearance::Appearance()
{
    QSettings s;
    const QString kind = s.value(kKindKey).toString();
    const QString color = s.value(kColorKey).toString();
    const QString mode = s.value(kModeKey).toString();

    if (kind.isEmpty() && s.contains(kLegacyThemeKey)) {
        // Migrate the old single "appearance/theme" = blue|graphite key.
        m_kind = Kind::Classic;
        m_classic = s.value(kLegacyThemeKey).toString() == QLatin1String("graphite") ? Theme::Gray
                                                                                      : Theme::Blue;
    } else if (kind.isEmpty()) {
        // Fresh install: the Modern look in Light mode is the default.
        m_kind = Kind::Modern;
        m_classic = Theme::Blue;
    } else {
        m_kind = kind == QLatin1String("modern") ? Kind::Modern : Kind::Classic;
        m_classic = color == QLatin1String("gray") ? Theme::Gray : Theme::Blue;
    }
    // Light is the default mode; "auto" and "dark" are only stored once chosen.
    m_mode = mode == QLatin1String("dark") ? Mode::Dark : mode == QLatin1String("auto") ? Mode::Auto : Mode::Light;
    m_resolved = m_classic;  // a safe value before install() resolves for real
}

void Appearance::install()
{
    QApplication::setStyle(new AppStyle);
    Appearance *a = instance();
    // The classic palette starts from whatever the base style provides.
    a->m_lightBasePalette = QApplication::palette();
    // Re-resolve whenever the system appearance flips, but only while on the
    // Modern look's Automatic setting.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, a,
                     [a](Qt::ColorScheme) {
                         if (a->m_kind == Kind::Modern && a->m_mode == Mode::Auto) a->resolve();
                     });
    a->resolve();
}

void Appearance::setKind(Kind kind)
{
    if (kind == m_kind) return;
    m_kind = kind;
    QSettings().setValue(kKindKey, kind == Kind::Modern ? QStringLiteral("modern") : QStringLiteral("classic"));
    resolve();
}

void Appearance::setClassicColor(Theme color)
{
    if (color != Theme::Blue && color != Theme::Gray) return;
    if (color == m_classic) return;
    m_classic = color;
    QSettings().setValue(kColorKey, color == Theme::Gray ? QStringLiteral("gray") : QStringLiteral("blue"));
    if (m_kind == Kind::Classic) resolve();
}

void Appearance::setMode(Mode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    QSettings().setValue(kModeKey, mode == Mode::Light   ? QStringLiteral("light")
                                   : mode == Mode::Dark  ? QStringLiteral("dark")
                                                         : QStringLiteral("auto"));
    if (m_kind == Kind::Modern) resolve();
}

void Appearance::resolve()
{
    // 1. Set (or clear) the OS colour-scheme override first, so a later read of
    //    colorScheme() for the Automatic setting reflects the real system value.
    QStyleHints *h = QGuiApplication::styleHints();
    if (m_kind == Kind::Modern && m_mode == Mode::Auto)
        h->setColorScheme(Qt::ColorScheme::Unknown);  // follow the system
    else if (m_kind == Kind::Modern && m_mode == Mode::Dark)
        h->setColorScheme(Qt::ColorScheme::Dark);
    else
        h->setColorScheme(Qt::ColorScheme::Light);  // Classic, or Modern Light

    // 2. Effective theme.
    if (m_kind == Kind::Classic) {
        m_resolved = m_classic;
    } else {
        const bool dark = m_mode == Mode::Dark ||
                          (m_mode == Mode::Auto && h->colorScheme() == Qt::ColorScheme::Dark);
        m_resolved = dark ? Theme::FlatDark : Theme::FlatLight;
    }

    applyPalette();
    // Scroll bars and chrome read the theme at paint time; repaint everything.
    for (QWidget *w : QApplication::allWidgets()) w->update();
    emit themeChanged(m_resolved);
}

void Appearance::applyPalette()
{
    if (!qApp) return;
    if (isFlat()) {
        QApplication::setPalette(flatPalette(m_resolved == Theme::FlatDark));
        return;
    }
    QPalette pal = m_lightBasePalette;
    const QColor hl = highlight();
    for (auto group : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(group, QPalette::Highlight, hl);
        pal.setColor(group, QPalette::HighlightedText, Qt::white);
        pal.setColor(group, QPalette::Accent, hl);
    }
    QApplication::setPalette(pal);
}

QColor Appearance::highlight() const
{
    switch (m_resolved) {
    case Theme::Gray: return QColor(0x75, 0x87, 0xa0);
    case Theme::FlatLight:
    case Theme::FlatDark: return QColor(0x0a, 0x84, 0xff);
    default: return QColor(0x38, 0x75, 0xd7);
    }
}

QColor Appearance::sidebarBackground() const
{
    switch (m_resolved) {
    case Theme::Gray: return QColor(0xe0, 0xe0, 0xe0);
    case Theme::FlatLight: return QColor(0xec, 0xee, 0xf2);
    case Theme::FlatDark: return QColor(0x20, 0x20, 0x22);
    default: return QColor(0xd6, 0xdd, 0xe5);
    }
}

QColor Appearance::windowChrome() const
{
    return m_resolved == Theme::FlatDark ? QColor(0x2a, 0x2a, 0x2c) : QColor(0xec, 0xec, 0xef);
}

QColor Appearance::chromeBorder() const
{
    return m_resolved == Theme::FlatDark ? QColor(0x0c, 0x0c, 0x0d) : QColor(0xd2, 0xd2, 0xd7);
}

QColor Appearance::hairline() const
{
    return m_resolved == Theme::FlatDark ? QColor(0x33, 0x33, 0x36) : QColor(0xe2, 0xe2, 0xe6);
}

QColor Appearance::scrollTrack() const
{
    return m_resolved == Theme::FlatDark ? QColor(0x20, 0x20, 0x22) : QColor(0xee, 0xf0, 0xf3);
}

QColor Appearance::scrollThumb() const
{
    return m_resolved == Theme::FlatDark ? QColor(0x48, 0x48, 0x4a) : QColor(0xc2, 0xc2, 0xc8);
}

QGradientStops Appearance::thumbStops(Theme theme)
{
    // A lit glass cylinder: dark rims, bright core.
    if (theme == Theme::Gray)
        return {{0.0, QColor(0x74, 0x7f, 0x8d)}, {0.14, QColor(0x9a, 0xa5, 0xb2)}, {0.5, QColor(0xd6, 0xdc, 0xe3)},
                {0.86, QColor(0xa4, 0xae, 0xba)}, {1.0, QColor(0x7a, 0x85, 0x93)}};
    return {{0.0, QColor(0x2f, 0x6a, 0xcc)}, {0.14, QColor(0x5a, 0x98, 0xe8)}, {0.5, QColor(0xb0, 0xd8, 0xfc)},
            {0.86, QColor(0x66, 0xa6, 0xef)}, {1.0, QColor(0x33, 0x70, 0xd2)}};
}

QColor Appearance::thumbOutline(Theme theme)
{
    return theme == Theme::Gray ? QColor(0x55, 0x5e, 0x6a) : QColor(0x24, 0x4f, 0x95);
}
