// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "AppStyle.h"

#include "Appearance.h"
#include "Preferences.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include <QWidget>

namespace {

// Flat scroll-bar arrow-button weights, 1 (lightest) to 4 (solid): the button
// fill and the arrow glyph, for the Light and Dark modern themes. These mirror
// the weights offered by XTunes+.
struct ArrowWeight {
    QColor fill, glyph;
};
const ArrowWeight kLightArrows[4] = {
    {QColor(0xdc, 0xeb, 0xff), QColor(0x0a, 0x84, 0xff)},
    {QColor(0xb6, 0xda, 0xff), QColor(0x0a, 0x84, 0xff)},
    {QColor(0x78, 0xbb, 0xff), QColor(0xff, 0xff, 0xff)},
    {QColor(0x0a, 0x84, 0xff), QColor(0xff, 0xff, 0xff)},
};
const ArrowWeight kDarkArrows[4] = {
    {QColor(0x36, 0x36, 0x38), QColor(0xe9, 0xe9, 0xee)},
    {QColor(0x50, 0x50, 0x53), QColor(0xe9, 0xe9, 0xee)},
    {QColor(0x7b, 0x7b, 0x7f), QColor(0x1c, 0x1c, 0x1e)},
    {QColor(0xc9, 0xc9, 0xcf), QColor(0x1c, 0x1c, 0x1e)},
};

constexpr int kScrollBarExtent = 15;
constexpr int kArrowLength = 16;
constexpr int kSliderMin = 24;

// Scroll bar layout, classic style: [track (rounded start cap)][sub arrow][add arrow].
struct ScrollGeometry {
    QRect groove, slider, subLine, addLine, subPage, addPage;
};

ScrollGeometry scrollGeometry(const QStyleOptionSlider *sb)
{
    const bool horiz = sb->orientation == Qt::Horizontal;
    const QRect r = sb->rect;
    const int len = horiz ? r.width() : r.height();
    const int thick = horiz ? r.height() : r.width();
    // Short bars shrink the arrows before the track disappears.
    const int arrow = qBound(0, (len - kSliderMin) / 2, kArrowLength);
    const int grooveLen = qMax(0, len - 2 * arrow);
    auto seg = [&](int start, int length) {
        return horiz ? QRect(r.x() + start, r.y(), length, thick) : QRect(r.x(), r.y() + start, thick, length);
    };

    ScrollGeometry g;
    g.groove = seg(0, grooveLen);
    g.subLine = seg(grooveLen, arrow);
    g.addLine = seg(grooveLen + arrow, arrow);
    const qint64 range = qint64(sb->maximum) - sb->minimum;
    int sliderLen = grooveLen;
    if (range > 0) {
        sliderLen = int(qint64(grooveLen) * sb->pageStep / qMax<qint64>(1, range + sb->pageStep));
        sliderLen = qBound(qMin(kSliderMin, grooveLen), sliderLen, grooveLen);
    }
    const int pos = QStyle::sliderPositionFromValue(sb->minimum, sb->maximum, sb->sliderPosition,
                                                    grooveLen - sliderLen, sb->upsideDown);
    g.slider = seg(pos, sliderLen);
    g.subPage = seg(0, pos);
    g.addPage = seg(pos + sliderLen, grooveLen - pos - sliderLen);
    return g;
}

QLinearGradient across(const QRectF &r, Qt::Orientation o)
{
    return o == Qt::Horizontal ? QLinearGradient(0, r.top(), 0, r.bottom()) : QLinearGradient(r.left(), 0, r.right(), 0);
}

void paintArrowButton(QPainter *p, const QRect &r, Qt::Orientation o, bool forward, bool pressed, bool enabled)
{
    if (r.isEmpty()) return;
    Appearance *ap = Appearance::instance();
    const bool flat = ap->isFlat();
    QColor glyph;
    if (flat) {
        // Flat: fill and glyph follow the chosen scroll-bar weight (1..4).
        const int w = qBound(1, Preferences::instance()->scrollbarStyle(), 4) - 1;
        const ArrowWeight &aw = ap->isDark() ? kDarkArrows[w] : kLightArrows[w];
        p->fillRect(r, pressed ? aw.fill.darker(120) : aw.fill);
        p->setPen(ap->hairline());
        if (o == Qt::Horizontal) p->drawLine(r.topLeft(), r.bottomLeft());
        else p->drawLine(r.topLeft(), r.topRight());
        glyph = enabled ? aw.glyph : ap->scrollThumb();
    } else {
        if (pressed) {
            QLinearGradient g = across(r, o);
            g.setStops(Appearance::thumbStops(ap->theme()));
            p->fillRect(r, g);
        } else {
            QLinearGradient g = across(r, o);
            g.setStops({{0.0, QColor(0xc4, 0xc4, 0xc4)}, {0.2, QColor(0xea, 0xea, 0xea)}, {0.55, QColor(0xfd, 0xfd, 0xfd)},
                        {1.0, QColor(0xe6, 0xe6, 0xe6)}});
            p->fillRect(r, g);
        }
        // Divider at the leading end of each button.
        p->setPen(QColor(0x8e, 0x8e, 0x8e));
        if (o == Qt::Horizontal) p->drawLine(r.topLeft(), r.bottomLeft());
        else p->drawLine(r.topLeft(), r.topRight());
        glyph = !enabled ? QColor(0xa8, 0xa8, 0xa8) : pressed ? QColor(0x10, 0x1c, 0x30) : QColor(0x24, 0x24, 0x24);
    }

    const QPointF c = QRectF(r).center() + (o == Qt::Horizontal ? QPointF(0.5, 0) : QPointF(0, 0.5));
    const double half = 3.5, depth = 4.0;
    QPolygonF tri;
    if (o == Qt::Vertical) {
        const double tip = forward ? depth / 2 : -depth / 2;
        tri << QPointF(c.x(), c.y() + tip) << QPointF(c.x() - half, c.y() - tip) << QPointF(c.x() + half, c.y() - tip);
    } else {
        const double tip = forward ? depth / 2 : -depth / 2;
        tri << QPointF(c.x() + tip, c.y()) << QPointF(c.x() - tip, c.y() - half) << QPointF(c.x() - tip, c.y() + half);
    }
    p->setRenderHint(QPainter::Antialiasing, flat);
    p->setPen(Qt::NoPen);
    p->setBrush(glyph);
    p->drawPolygon(tri);
    p->setRenderHint(QPainter::Antialiasing, false);
}

void drawScrollBar(const QStyleOptionSlider *sb, QPainter *p)
{
    const ScrollGeometry g = scrollGeometry(sb);
    const Qt::Orientation o = sb->orientation;
    const bool horiz = o == Qt::Horizontal;
    const bool enabled = (sb->state & QStyle::State_Enabled) && sb->maximum > sb->minimum;
    const bool sunken = sb->state & QStyle::State_Sunken;
    Appearance *ap = Appearance::instance();

    // Flat look: a plain track, a solid rounded thumb and flat arrow buttons.
    if (ap->isFlat()) {
        p->save();
        p->fillRect(sb->rect, ap->scrollTrack());
        if (enabled && !g.slider.isEmpty()) {
            const QRectF thumb = horiz ? QRectF(g.slider).adjusted(2, 3, -2, -3)
                                       : QRectF(g.slider).adjusted(3, 2, -3, -2);
            AppStyle::paintThumb(p, thumb, o, ap->theme(), sunken && (sb->activeSubControls & QStyle::SC_ScrollBarSlider));
        }
        const bool subDownF = sunken && (sb->activeSubControls & QStyle::SC_ScrollBarSubLine);
        const bool addDownF = sunken && (sb->activeSubControls & QStyle::SC_ScrollBarAddLine);
        paintArrowButton(p, g.subLine, o, false, subDownF, enabled);
        paintArrowButton(p, g.addLine, o, true, addDownF, enabled);
        p->setPen(ap->hairline());
        if (horiz) p->drawLine(sb->rect.topLeft(), sb->rect.topRight());
        else p->drawLine(sb->rect.topLeft(), sb->rect.bottomLeft());
        p->restore();
        return;
    }

    p->save();
    // Light frame behind the inset track (shows at the rounded cap's corners).
    p->fillRect(sb->rect, QColor(0xee, 0xee, 0xee));

    p->setRenderHint(QPainter::Antialiasing);
    if (!g.groove.isEmpty()) {
        // Inset well: rounded at the start, open towards the arrows.
        const QRectF well = QRectF(g.groove).adjusted(0.5, 0.5, -0.5, -0.5);
        const double radius = (horiz ? well.height() : well.width()) / 2;
        const QRectF shape = horiz ? well.adjusted(0, 0, radius * 2, 0) : well.adjusted(0, 0, 0, radius * 2);
        QPainterPath path;
        path.addRoundedRect(shape, radius, radius);
        p->save();
        p->setClipRect(g.groove);
        QLinearGradient tg = across(well, o);
        tg.setStops({{0.0, QColor(0xa4, 0xa4, 0xa4)}, {0.12, QColor(0xc6, 0xc6, 0xc6)}, {0.35, QColor(0xe2, 0xe2, 0xe2)},
                     {0.75, QColor(0xf3, 0xf3, 0xf3)}, {1.0, QColor(0xf8, 0xf8, 0xf8)}});
        p->setBrush(tg);
        p->setPen(QPen(QColor(0x9a, 0x9a, 0x9a), 1));
        p->drawPath(path);
        p->restore();
    }

    if (enabled && !g.slider.isEmpty()) {
        const QRectF thumb = horiz ? QRectF(g.slider).adjusted(0, 1, 0, -1) : QRectF(g.slider).adjusted(1, 0, -1, 0);
        AppStyle::paintThumb(p, thumb, o, Appearance::instance()->theme(), sunken && (sb->activeSubControls & QStyle::SC_ScrollBarSlider));
    }

    p->setRenderHint(QPainter::Antialiasing, false);
    const bool subDown = sunken && (sb->activeSubControls & QStyle::SC_ScrollBarSubLine);
    const bool addDown = sunken && (sb->activeSubControls & QStyle::SC_ScrollBarAddLine);
    paintArrowButton(p, g.subLine, o, false, subDown, enabled);
    paintArrowButton(p, g.addLine, o, true, addDown, enabled);

    // Hairline against the content.
    p->setPen(QColor(0xa0, 0xa0, 0xa0));
    if (horiz) p->drawLine(sb->rect.topLeft(), sb->rect.topRight());
    else p->drawLine(sb->rect.topLeft(), sb->rect.bottomLeft());
    p->restore();
}

}  // namespace

AppStyle::AppStyle(QStyle *base) : QProxyStyle(base) {}

void AppStyle::paintThumb(QPainter *p, const QRectF &r, Qt::Orientation o, Appearance::Theme theme, bool pressed)
{
    if (r.isEmpty()) return;
    // Flat look: a solid rounded capsule, no outline or gloss.
    if (theme == Appearance::Theme::FlatLight || theme == Appearance::Theme::FlatDark) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QColor fill = theme == Appearance::Theme::FlatDark ? QColor(0x48, 0x48, 0x4a) : QColor(0xc2, 0xc2, 0xc8);
        if (pressed) fill = fill.darker(120);
        const double radius = qMin(r.width(), r.height()) / 2;
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(r, radius, radius);
        p->restore();
        return;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF body = r.adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = qMin(body.width(), body.height()) / 2;

    QLinearGradient g = across(body, o);
    QGradientStops stops = Appearance::thumbStops(theme);
    if (pressed)
        for (auto &s : stops) s.second = s.second.darker(115);
    g.setStops(stops);
    p->setPen(QPen(Appearance::thumbOutline(theme), 1));
    p->setBrush(g);
    p->drawRoundedRect(body, radius, radius);

    // Specular gloss along the leading half.
    const QRectF gloss = o == Qt::Horizontal
                             ? QRectF(body.left() + radius * 0.5, body.top() + 1.5, body.width() - radius, body.height() * 0.42)
                             : QRectF(body.left() + 1.5, body.top() + radius * 0.5, body.width() * 0.42, body.height() - radius);
    QLinearGradient gg = across(gloss, o);
    gg.setColorAt(0.0, QColor(255, 255, 255, 200));
    gg.setColorAt(1.0, QColor(255, 255, 255, 30));
    p->setPen(Qt::NoPen);
    p->setBrush(gg);
    const double gr = qMin(gloss.width(), gloss.height()) / 2;
    p->drawRoundedRect(gloss, gr, gr);
    p->restore();
}

void AppStyle::paintTitleGradient(QPainter *p, const QRect &r, int spanTop, int spanBottom, bool active)
{
    if (Appearance::instance()->isFlat()) {
        // Flat chrome: a solid fill, dimmed a touch when the window is inactive.
        QColor c = Appearance::instance()->windowChrome();
        if (!active) c = c.lighter(Appearance::instance()->isDark() ? 112 : 102);
        p->fillRect(r, c);
        return;
    }
    QLinearGradient g(0, spanTop, 0, spanBottom);
    if (active) {
        g.setColorAt(0.0, QColor(0xe8, 0xe8, 0xe8));
        g.setColorAt(1.0, QColor(0xa9, 0xa9, 0xa9));
    } else {
        g.setColorAt(0.0, QColor(0xf5, 0xf5, 0xf5));
        g.setColorAt(1.0, QColor(0xd4, 0xd4, 0xd4));
    }
    p->fillRect(r, g);
}

int AppStyle::pixelMetric(PixelMetric metric, const QStyleOption *opt, const QWidget *w) const
{
    switch (metric) {
    case PM_ScrollBarExtent: return kScrollBarExtent;
    case PM_ScrollBarSliderMin: return kSliderMin;
    default: return QProxyStyle::pixelMetric(metric, opt, w);
    }
}

int AppStyle::styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const
{
    // Classic, always-visible scroll bars (no macOS overlay scrollers).
    if (hint == SH_ScrollBar_Transient) return 0;
    return QProxyStyle::styleHint(hint, opt, w, ret);
}

QSize AppStyle::sizeFromContents(ContentsType type, const QStyleOption *opt, const QSize &size, const QWidget *w) const
{
    if (type == CT_ScrollBar) return size;
    return QProxyStyle::sizeFromContents(type, opt, size, w);
}

QRect AppStyle::subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc, const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        if (const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt)) {
            const ScrollGeometry g = scrollGeometry(sb);
            switch (sc) {
            case SC_ScrollBarGroove: return g.groove;
            case SC_ScrollBarSlider: return g.slider;
            case SC_ScrollBarSubLine: return g.subLine;
            case SC_ScrollBarAddLine: return g.addLine;
            case SC_ScrollBarSubPage: return g.subPage;
            case SC_ScrollBarAddPage: return g.addPage;
            default: return {};
            }
        }
    }
    return QProxyStyle::subControlRect(cc, opt, sc, w);
}

QStyle::SubControl AppStyle::hitTestComplexControl(ComplexControl cc, const QStyleOptionComplex *opt,
                                                    const QPoint &pt, const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        if (const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt)) {
            const ScrollGeometry g = scrollGeometry(sb);
            const std::pair<QRect, SubControl> parts[] = {
                {g.slider, SC_ScrollBarSlider},   {g.subLine, SC_ScrollBarSubLine}, {g.addLine, SC_ScrollBarAddLine},
                {g.subPage, SC_ScrollBarSubPage}, {g.addPage, SC_ScrollBarAddPage}, {g.groove, SC_ScrollBarGroove}};
            for (const auto &[rect, part] : parts)
                if (rect.contains(pt)) return part;
            return SC_None;
        }
    }
    return QProxyStyle::hitTestComplexControl(cc, opt, pt, w);
}

void AppStyle::drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                                   const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        if (const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt)) {
            drawScrollBar(sb, p);
            return;
        }
    }
    QProxyStyle::drawComplexControl(cc, opt, p, w);
}

void AppStyle::drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    if (ce == CE_ToolBar && w) {
        // Continues the gradient painted behind the title bar.
        const bool active = w->isActiveWindow();
        const int top = w->mapTo(w->window(), QPoint(0, 0)).y();
        paintTitleGradient(p, opt->rect, -top, w->height(), active);
        if (Appearance::instance()->isFlat())
            p->setPen(Appearance::instance()->chromeBorder());
        else
            p->setPen(active ? QColor(0x51, 0x51, 0x51) : QColor(0x87, 0x87, 0x87));
        p->drawLine(opt->rect.bottomLeft(), opt->rect.bottomRight());
        return;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

void AppStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    if (pe == PE_PanelStatusBar) {
        // Classic bottom bar.
        const bool active = w ? w->isActiveWindow() : bool(opt->state & State_Active);
        const QRect r = opt->rect;
        if (Appearance::instance()->isFlat()) {
            p->fillRect(r, Appearance::instance()->windowChrome());
            p->setPen(Appearance::instance()->chromeBorder());
            p->drawLine(r.topLeft(), r.topRight());
            return;
        }
        QLinearGradient g(0, r.top(), 0, r.bottom());
        g.setColorAt(0.0, active ? QColor(0xcb, 0xcb, 0xcb) : QColor(0xeb, 0xeb, 0xeb));
        g.setColorAt(1.0, active ? QColor(0xa7, 0xa7, 0xa7) : QColor(0xd6, 0xd6, 0xd6));
        p->fillRect(r, g);
        p->setPen(active ? QColor(0x51, 0x51, 0x51) : QColor(0x99, 0x99, 0x99));
        p->drawLine(r.topLeft(), r.topRight());
        p->setPen(QColor(255, 255, 255, active ? 110 : 70));
        p->drawLine(r.topLeft() + QPoint(0, 1), r.topRight() + QPoint(0, 1));
        return;
    }
    if (pe == PE_FrameStatusBarItem) return;  // no item boxes on the gradient
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}
