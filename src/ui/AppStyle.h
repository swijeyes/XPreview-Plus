// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "Appearance.h"

#include <QProxyStyle>

// Wraps the platform style and replaces a few elements with the app's own
// look: for the Classic theme, glossy scroll bars (a lit thumb, both arrows
// together at the end) and gradient window chrome; for the Modern theme, flat
// scroll bars and chrome. Follows the current Appearance theme.
class AppStyle : public QProxyStyle {
    Q_OBJECT
public:
    explicit AppStyle(QStyle *base = nullptr);

    int pixelMetric(PixelMetric metric, const QStyleOption *opt, const QWidget *w) const override;
    int styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *opt, const QSize &size,
                           const QWidget *w) const override;
    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc,
                         const QWidget *w) const override;
    SubControl hitTestComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, const QPoint &pt,
                                     const QWidget *w) const override;
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w) const override;
    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const override;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override;

    // Glossy capsule (scroller thumb, theme swatches).
    static void paintThumb(QPainter *p, const QRectF &r, Qt::Orientation orientation, Appearance::Theme theme,
                           bool pressed = false);
    // The unified title bar + toolbar gradient. `spanTop`/`spanBottom` give the
    // vertical extent of the whole gradient in painter coordinates, so pieces
    // painted by different widgets line up.
    static void paintTitleGradient(QPainter *p, const QRect &r, int spanTop, int spanBottom, bool active);
};
