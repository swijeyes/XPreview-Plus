// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "DocumentView.h"

#include "RenderService.h"
#include "core/Exporter.h"
#include "core/Limits.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QMessageBox>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyle>
#include <QUrl>
#include <QtMath>

namespace {

constexpr double kMargin = 16;
constexpr double kGap = 14;
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 32.0;
const QList<double> kZoomSteps = {0.1, 0.125, 0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25,
                                  1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0};

}  // namespace

DocumentView::DocumentView(DocumentPtr doc, RenderService *renderer, QWidget *parent)
    : QAbstractScrollArea(parent), m_doc(std::move(doc)), m_renderer(renderer)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->grabGesture(Qt::PinchGesture);
    setFocusPolicy(Qt::StrongFocus);
    verticalScrollBar()->setSingleStep(24);
    horizontalScrollBar()->setSingleStep(24);
    connect(renderer, &RenderService::tileReady, viewport(), qOverload<>(&QWidget::update));
    connect(renderer, &RenderService::thumbnailReady, viewport(), qOverload<>(&QWidget::update));

    // Images open at actual size unless they do not fit; documents fit width.
    if (m_doc->kind() == DocumentKind::Image || m_doc->kind() == DocumentKind::Svg)
        m_fit = FitMode::Page;
    relayout();
}

QSizeF DocumentView::displaySize(int page) const
{
    return Exporter::rotatedSize(m_doc->pageSize(page), m_rotation) * m_zoom;
}

void DocumentView::relayout()
{
    const int n = m_doc->pageCount();
    m_tops.resize(n);
    double y = kMargin, maxW = 0;
    for (int i = 0; i < n; ++i) {
        const QSizeF s = displaySize(i);
        m_tops[i] = y;
        y += s.height() + kGap;
        maxW = qMax(maxW, s.width());
    }
    m_content = QSizeF(maxW + 2 * kMargin, y - kGap + kMargin);
    updateScrollBars();
    viewport()->update();
}

void DocumentView::updateScrollBars()
{
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, qMax(0, qCeil(m_content.width()) - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, qMax(0, qCeil(m_content.height()) - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

QRectF DocumentView::pageRect(int page) const
{
    const QSizeF s = displaySize(page);
    const double contentW = qMax(m_content.width(), double(viewport()->width()));
    return QRectF((contentW - s.width()) / 2, m_tops.value(page), s.width(), s.height());
}

QTransform DocumentView::pageTransform(int page) const
{
    const QRectF r = pageRect(page);
    QTransform t;
    t.translate(qRound(r.x() - horizontalScrollBar()->value()), qRound(r.y() - verticalScrollBar()->value()));
    // Rotation inside the page box, then points -> logical px.
    const QSizeF s = m_doc->pageSize(page) * m_zoom;
    switch (m_rotation) {
    case 90: t.translate(s.height(), 0); break;
    case 180: t.translate(s.width(), s.height()); break;
    case 270: t.translate(0, s.width()); break;
    default: break;
    }
    t.rotate(m_rotation);
    t.scale(m_zoom, m_zoom);
    return t;
}

int DocumentView::pageAt(QPointF pos) const
{
    const double y = pos.y() + verticalScrollBar()->value();
    for (int i = 0; i < m_tops.size(); ++i) {
        const QRectF r = pageRect(i);
        if (y < r.bottom() + kGap / 2) return i;
    }
    return m_tops.size() - 1;
}

QPointF DocumentView::toPage(int page, QPointF pos) const
{
    return pageTransform(page).inverted().map(pos);
}

void DocumentView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    const QColor bg = palette().color(QPalette::Window).lightness() > 128
                          ? QColor(0xe4, 0xe4, 0xe6)
                          : QColor(0x2a, 0x2a, 0x2c);
    p.fillRect(event->rect(), bg);
    if (!m_renderer) return;

    const double dpr = viewport()->devicePixelRatioF();
    const double scale = m_zoom * dpr;  // device px per point
    const int T = RenderService::kTileSize;
    const QRectF vis = QRectF(event->rect()).translated(horizontalScrollBar()->value(),
                                                        verticalScrollBar()->value());
    for (int i = 0; i < m_tops.size(); ++i) {
        const QRectF content = pageRect(i);
        if (content.bottom() < vis.top()) continue;
        if (content.top() > vis.bottom()) break;
        const QRectF vr = content.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

        // Paper and shadow
        p.setPen(Qt::NoPen);
        p.fillRect(vr.adjusted(-1, 0, 1, 2), QColor(0, 0, 0, 40));
        p.fillRect(vr, Qt::white);

        const QSizeF pageSize = m_doc->pageSize(i);
        const QTransform t = pageTransform(i);
        const QRectF visPt = t.inverted().mapRect(QRectF(event->rect()).intersected(vr))
                                 .intersected(QRectF(QPointF(0, 0), pageSize));
        if (visPt.isEmpty()) continue;

        p.save();
        p.setTransform(t);
        // Tiles cover the page in unrotated device pixels.
        const int tx0 = qFloor(visPt.left() * scale / T), tx1 = qFloor((visPt.right() - 0.001) * scale / T);
        const int ty0 = qFloor(visPt.top() * scale / T), ty1 = qFloor((visPt.bottom() - 0.001) * scale / T);
        struct Ready { QRectF rect; QImage img; };
        QVector<Ready> ready;
        bool missing = false;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                const QImage img = m_renderer->tile(i, scale, tx, ty);
                if (img.isNull()) {
                    missing = true;
                    continue;
                }
                ready << Ready{QRectF(tx * T / scale, ty * T / scale, img.width() / scale, img.height() / scale), img};
            }
        }
        if (missing) {
            // Placeholder: a cheap whole-page render, scaled.
            const int w = qBound(64, qRound(pageSize.width() * qMin(scale, 1.0)), 1024);
            const QImage thumb = m_renderer->thumbnail(i, w);
            if (!thumb.isNull()) {
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawImage(QRectF(QPointF(0, 0), pageSize), thumb);
            }
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        for (const Ready &r : std::as_const(ready)) p.drawImage(r.rect, r.img);

        // Search hits and selection overlays (page points).
        p.setPen(Qt::NoPen);
        for (int h = 0; h < m_hits.size(); ++h) {
            const QColor c = h == m_currentHit ? QColor(255, 150, 0, 140) : QColor(255, 220, 0, 100);
            for (const PageRect &b : m_hits[h].boxes)
                if (b.page == i) p.fillRect(b.rect.adjusted(-0.5, -0.5, 0.5, 0.5), c);
        }
        for (const PageRect &b : m_selection.boxes)
            if (b.page == i) p.fillRect(b.rect, QColor(0, 110, 255, 70));
        p.restore();
    }
}

void DocumentView::applyFit()
{
    if (m_fit == FitMode::None || m_doc->pageCount() == 0) return;
    // Fit against the area *without* scrollbars, always reserving the
    // vertical one. The result then cannot depend on whether a scrollbar is
    // currently shown; using the live viewport made the zoom oscillate with
    // classic scrollbars (zoom -> scrollbar appears -> viewport shrinks ->
    // zoom changes -> scrollbar disappears -> ...).
    const QSize full = maximumViewportSize();
    const int sb = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
    double maxW = 0;
    for (int i = 0; i < m_doc->pageCount(); ++i)
        maxW = qMax(maxW, Exporter::rotatedSize(m_doc->pageSize(i), m_rotation).width());
    const double availW = qMax(50.0, full.width() - sb - 2 * kMargin);
    double z = availW / qMax(1.0, maxW);
    if (m_fit == FitMode::Page) {
        const QSizeF s = Exporter::rotatedSize(m_doc->pageSize(m_currentPage), m_rotation);
        z = qMin(z, qMax(50.0, full.height() - 2 * kMargin) / qMax(1.0, s.height()));
        // Small images are not blown up.
        if (m_doc->kind() == DocumentKind::Image || m_doc->kind() == DocumentKind::Svg) z = qMin(z, 1.0);
    }
    z = qBound(kMinZoom, z, maxZoom());
    if (qAbs(z - m_zoom) > 1e-4) {
        const int page = m_currentPage;
        const double frac = [&] {
            const QRectF r = pageRect(page);
            return r.height() > 0 ? (verticalScrollBar()->value() - r.top()) / r.height() : 0.0;
        }();
        m_zoom = z;
        if (m_renderer) m_renderer->cancelPending();
        relayout();
        const QRectF r = pageRect(page);
        verticalScrollBar()->setValue(qRound(r.top() + frac * r.height()));
        emit zoomChanged(m_zoom);
    }
}

double DocumentView::maxZoom() const
{
    // Keep the scrollable content within int range for any page count/size.
    double total = 0, widest = 0;
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const QSizeF s = Exporter::rotatedSize(m_doc->pageSize(i), m_rotation);
        total += s.height();
        widest = qMax(widest, s.width());
    }
    const double limit = Limits::kMaxContentPixels / qMax(1.0, qMax(total, widest));
    return qMax(kMinZoom, qMin(kMaxZoom, limit));
}

void DocumentView::setZoom(double zoom, QPointF anchor)
{
    zoom = qBound(kMinZoom, zoom, maxZoom());
    m_fit = FitMode::None;
    if (qAbs(zoom - m_zoom) < 1e-5) return;
    if (anchor.x() < 0) anchor = QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0);
    const int page = pageAt(anchor);
    const QPointF pt = toPage(page, anchor);
    m_zoom = zoom;
    if (m_renderer) m_renderer->cancelPending();
    relayout();
    // Keep the point under the anchor fixed.
    const QPointF now = pageTransform(page).map(pt);
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(now.x() - anchor.x()));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(now.y() - anchor.y()));
    updateCurrentPage();
    emit zoomChanged(m_zoom);
}

void DocumentView::zoomIn()
{
    for (double s : kZoomSteps)
        if (s > m_zoom * 1.01) return setZoom(s);
    setZoom(kMaxZoom);
}

void DocumentView::zoomOut()
{
    for (auto it = kZoomSteps.crbegin(); it != kZoomSteps.crend(); ++it)
        if (*it < m_zoom * 0.99) return setZoom(*it);
    setZoom(kMinZoom);
}

void DocumentView::actualSize() { setZoom(1.0); }

void DocumentView::setFitMode(FitMode mode)
{
    m_fit = mode;
    applyFit();
}

void DocumentView::rotateBy(int degrees)
{
    const int page = m_currentPage;
    m_rotation = ((m_rotation + degrees) % 360 + 360) % 360;
    if (m_renderer) m_renderer->cancelPending();
    relayout();
    applyFit();
    goToPage(page);
}

void DocumentView::goToPage(int page, double yPoints)
{
    if (page < 0 || page >= m_tops.size()) return;
    const QRectF r = pageRect(page);
    double y = r.top() - kMargin;
    if (yPoints >= 0 && m_rotation == 0) y = r.top() + yPoints * m_zoom - kMargin * 2;
    verticalScrollBar()->setValue(qRound(y));
    updateCurrentPage();
}

void DocumentView::setHits(const QVector<TextHit> &hits)
{
    m_hits = hits;
    m_currentHit = -1;
    viewport()->update();
}

void DocumentView::setCurrentHit(int index)
{
    m_currentHit = index;
    if (index >= 0 && index < m_hits.size() && !m_hits[index].boxes.isEmpty()) {
        const PageRect &b = m_hits[index].boxes.first();
        const QRectF vr = pageTransform(b.page).mapRect(b.rect);
        const QRect view = viewport()->rect().adjusted(20, 40, -20, -40);
        if (!view.contains(vr.toRect())) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(vr.center().y() - view.center().y()));
            if (vr.left() < view.left() || vr.right() > view.right())
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() +
                                                qRound(vr.center().x() - view.center().x()));
        }
    }
    viewport()->update();
}

void DocumentView::clearSelection()
{
    if (m_selection.boxes.isEmpty() && m_selection.text.isEmpty()) return;
    m_selection = {};
    viewport()->update();
    emit selectionChanged();
}

void DocumentView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
    applyFit();
}

void DocumentView::scrollContentsBy(int, int)
{
    viewport()->update();
    updateCurrentPage();
}

void DocumentView::updateCurrentPage()
{
    // The page occupying the upper-middle of the viewport is "current"; at
    // the very top it is always the first page (also guards the tiny
    // layouts a widget goes through before it is first shown).
    if (!isVisible() || viewport()->height() < 50) return;
    const int page = verticalScrollBar()->value() == 0
                         ? 0
                         : pageAt(QPointF(0, qMin(viewport()->height() * 0.35, 200.0)));
    if (page != m_currentPage && page >= 0) {
        m_currentPage = page;
        emit currentPageChanged(page);
    }
}

void DocumentView::updateCursor(QPointF pos)
{
    if (m_panning) {
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    const int page = pageAt(pos);
    const QPointF pt = toPage(page, pos);
    const QSizeF size = m_doc->pageSize(page);
    const bool onPage = QRectF(QPointF(0, 0), size).contains(pt);
    if (onPage && m_doc->hasText() && !m_doc->linkAt(page, pt).isEmpty())
        viewport()->setCursor(Qt::PointingHandCursor);
    else if (onPage && m_doc->hasText())
        viewport()->setCursor(Qt::IBeamCursor);
    else if (horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0)
        viewport()->setCursor(m_doc->hasText() ? Qt::ArrowCursor : Qt::OpenHandCursor);
    else
        viewport()->setCursor(Qt::ArrowCursor);
}

void DocumentView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return QAbstractScrollArea::mousePressEvent(e);
    m_pressPos = e->position();
    m_pressPage = pageAt(e->position());
    m_pressPagePt = toPage(m_pressPage, e->position());
    m_dragging = false;
    if (!m_doc->hasText() || (e->modifiers() & Qt::AltModifier)) {
        m_panning = true;
        m_panStart = e->position().toPoint();
        m_scrollStart = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        updateCursor(e->position());
    }
}

void DocumentView::mouseMoveEvent(QMouseEvent *e)
{
    if (!(e->buttons() & Qt::LeftButton)) {
        updateCursor(e->position());
        return;
    }
    if (m_panning) {
        const QPoint d = e->position().toPoint() - m_panStart;
        horizontalScrollBar()->setValue(m_scrollStart.x() - d.x());
        verticalScrollBar()->setValue(m_scrollStart.y() - d.y());
        return;
    }
    if (!m_dragging && (e->position() - m_pressPos).manhattanLength() < 4) return;
    m_dragging = true;
    const QPointF pt = toPage(m_pressPage, e->position());
    m_selection = m_doc->selectText(m_pressPage, m_pressPagePt, pt);
    viewport()->update();
    emit selectionChanged();
}

void DocumentView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return QAbstractScrollArea::mouseReleaseEvent(e);
    const bool wasPanning = m_panning;
    m_panning = false;
    updateCursor(e->position());
    if (m_dragging) {
        m_dragging = false;
        return;
    }
    if ((e->position() - m_pressPos).manhattanLength() >= 4 && wasPanning) return;
    clearSelection();
    if (m_doc->hasText()) {
        const QString link = m_doc->linkAt(m_pressPage, m_pressPagePt);
        if (!link.isEmpty()) openLink(link);
    }
}

void DocumentView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (m_doc->hasText()) return QAbstractScrollArea::mouseDoubleClickEvent(e);
    // Double-click toggles between fit and actual size for images.
    if (m_fit == FitMode::None) setFitMode(FitMode::Page);
    else setZoom(1.0, e->position());
}

void DocumentView::openLink(const QString &link)
{
    if (link.startsWith(QLatin1String("#page="))) {
        const int page = link.mid(6).toInt();
        goToPage(page);
        emit internalLinkActivated(page);
        return;
    }
    if (link.startsWith(QLatin1Char('#'))) return;  // bookmark in flow document
    const QUrl url(link);
    const QString scheme = url.scheme().toLower();
    // Only well-known schemes, and only on explicit user action.
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https") &&
        scheme != QLatin1String("mailto"))
        return;
    QDesktopServices::openUrl(url);
}

void DocumentView::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const double factor = qPow(1.0015, e->angleDelta().y());
        setZoom(m_zoom * factor, e->position());
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

bool DocumentView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        const QPointF pos = viewport()->mapFromGlobal(g->globalPosition());
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(m_zoom * (1.0 + g->value()), pos);
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            if (m_fit == FitMode::Width) setZoom(m_zoom * 2, pos);
            else setFitMode(FitMode::Width);
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void DocumentView::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Home: goToPage(0); return;
    case Qt::Key_End: goToPage(m_doc->pageCount() - 1); return;
    case Qt::Key_Space:
        verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                      (e->modifiers() & Qt::ShiftModifier ? -1 : 1) *
                                          int(viewport()->height() * 0.9));
        return;
    case Qt::Key_Left:
        if (horizontalScrollBar()->maximum() == 0) { goToPage(m_currentPage - 1); return; }
        break;
    case Qt::Key_Right:
        if (horizontalScrollBar()->maximum() == 0) { goToPage(m_currentPage + 1); return; }
        break;
    case Qt::Key_Escape: clearSelection(); return;
    default: break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}
