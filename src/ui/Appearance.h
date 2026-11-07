// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QColor>
#include <QGradientStops>
#include <QObject>
#include <QPalette>

// The user-selectable appearance and the colours derived from it.
//
// Two looks are offered. "Classic" is the glossy chrome in either Blue or
// Gray. "Modern" is a flat look in Light or Dark, with an Automatic setting
// that follows the system appearance. Modern's dark side comes in two styles:
// neutral Graphite (mirroring XTunes+) and Dracula (draculatheme.com).
// Persisted in QSettings under "appearance/".
class Appearance : public QObject {
    Q_OBJECT
public:
    // The resolved theme that painting code switches on. Blue and Gray are
    // the two Classic variants; FlatLight, FlatDark (Graphite) and
    // FlatDracula are the Modern ones.
    enum class Theme { Blue, Gray, FlatLight, FlatDark, FlatDracula };
    // Top-level choice.
    enum class Kind { Classic, Modern };
    // Light/dark selection for the Modern look.
    enum class Mode { Auto, Light, Dark };
    // How the Modern look is dark (whenever it is: Dark, or Automatic at night).
    enum class DarkStyle { Graphite, Dracula };
    // Fill and arrow colour of a flat scroll-bar arrow button.
    struct ArrowColors {
        QColor fill, glyph;
    };

    static Appearance *instance();
    // Installs AppStyle and the theme palette; call once after QApplication.
    static void install();

    Kind kind() const { return m_kind; }
    // The Classic colour variant (Blue or Gray), remembered even while the
    // Modern look is active.
    Theme classicColor() const { return m_classic; }
    Mode mode() const { return m_mode; }
    DarkStyle darkStyle() const { return m_darkStyle; }
    void setKind(Kind kind);
    void setClassicColor(Theme color);  // Blue or Gray
    void setMode(Mode mode);
    void setDarkStyle(DarkStyle style);

    // The resolved, effective theme.
    Theme theme() const { return m_resolved; }
    bool isFlat() const { return isFlat(m_resolved); }
    static bool isFlat(Theme t) { return t == Theme::FlatLight || t == Theme::FlatDark || t == Theme::FlatDracula; }
    bool isDark() const { return m_resolved == Theme::FlatDark || m_resolved == Theme::FlatDracula; }
    bool isDracula() const { return m_resolved == Theme::FlatDracula; }

    QColor highlight() const;
    QColor highlightedText() const;
    // Accent for focus rings, the current tab and similar marks.
    QColor accent() const;
    QColor sidebarBackground() const;
    // Behind the pages in the page view.
    QColor pageViewBackground() const;
    QColor text() const;
    QColor dimText() const;
    QColor fieldBackground() const;  // text fields such as the search box
    // Scroll-bar arrow button at a weight from 1 (lightest) to 4 (solid).
    ArrowColors arrowColors(int weight) const;
    // Flat-look chrome colours (window bar, separators, scroll bars). Only
    // meaningful while isFlat(); the Classic chrome is drawn with gradients.
    QColor windowChrome() const;
    QColor chromeBorder() const;
    QColor hairline() const;
    QColor scrollTrack() const;
    QColor scrollThumb() const;

    // Stops across the thickness of a scroller thumb / pressed arrow (Classic).
    static QGradientStops thumbStops(Theme theme);
    static QColor thumbOutline(Theme theme);

signals:
    void themeChanged(Appearance::Theme theme);

private:
    Appearance();
    void applyPalette();
    // Recompute the effective theme from the current choice, push the palette
    // and colour scheme, and repaint.
    void resolve();

    Kind m_kind = Kind::Classic;
    Theme m_classic = Theme::Blue;  // Blue or Gray only
    Mode m_mode = Mode::Auto;
    DarkStyle m_darkStyle = DarkStyle::Graphite;
    Theme m_resolved = Theme::Blue;
    QPalette m_lightBasePalette;
};
