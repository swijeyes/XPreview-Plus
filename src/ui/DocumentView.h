// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QAbstractScrollArea>
#include <QPointer>

class RenderService;

// Continuous vertical page view. Pages are drawn from asynchronously
// rendered tiles at the current zoom and device pixel ratio, with a
// low-resolution whole-page image as a placeholder while tiles arrive.
class DocumentView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class FitMode { None, Width, Page };

    DocumentView(DocumentPtr doc, RenderService *renderer, QWidget *parent = nullptr);

    double zoom() const { return m_zoom; }
    void setZoom(double zoom, QPointF anchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void actualSize();
    FitMode fitMode() const { return m_fit; }
    void setFitMode(FitMode mode);

    int rotation() const { return m_rotation; }
    void rotateBy(int degrees);

    int currentPage() const { return m_currentPage; }
    void goToPage(int page, double yPoints = -1);

    void setHits(const QVector<TextHit> &hits);
    void setCurrentHit(int index);  // scrolls it into view

    bool hasSelection() const { return !m_selection.text.isEmpty(); }
    QString selectedText() const { return m_selection.text; }
    void clearSelection();

signals:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void selectionChanged();
    void internalLinkActivated(int page);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool viewportEvent(QEvent *) override;

private:
    void relayout();
    double maxZoom() const;
    void applyFit();
    void updateScrollBars();
    void updateCurrentPage();
    QSizeF displaySize(int page) const;     // rotated, in logical px
    QRectF pageRect(int page) const;         // content coordinates
    QTransform pageTransform(int page) const;  // page points -> viewport
    int pageAt(QPointF viewportPos) const;
    QPointF toPage(int page, QPointF viewportPos) const;
    void updateCursor(QPointF pos);
    void openLink(const QString &link);

    DocumentPtr m_doc;
    QPointer<RenderService> m_renderer;
    double m_zoom = 1.0;
    FitMode m_fit = FitMode::Width;
    int m_rotation = 0;
    int m_currentPage = 0;
    QVector<double> m_tops;  // page tops in content px
    QSizeF m_content;

    QVector<TextHit> m_hits;
    int m_currentHit = -1;

    // Mouse state
    QPointF m_pressPos;
    int m_pressPage = -1;
    QPointF m_pressPagePt;
    bool m_dragging = false;
    bool m_panning = false;
    QPoint m_panStart;
    QPoint m_scrollStart;
    TextHit m_selection;
};
