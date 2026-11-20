// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"
#include "pdf/PdfDocument.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPointer>

#include <utility>

class RenderService;

// Page view: continuous vertical scroll, one page, or two pages side by
// side. Pages are drawn from asynchronously rendered tiles at the current
// zoom and device pixel ratio, with a low-resolution whole-page image as a
// placeholder while tiles arrive.
class DocumentView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class FitMode { None, Width, Page };
    enum class DisplayMode { Continuous, SinglePage, TwoPages };
    // What a click or drag on a PDF page does. Every tool but Select adds
    // annotations (when the PDF's owner allows it).
    enum class Tool { Select, Highlight, Underline, StrikeOut, Note, Draw, Rectangle, Oval, TextBox };

    DocumentView(DocumentPtr doc, RenderService *renderer, QWidget *parent = nullptr);

    double zoom() const { return m_zoom; }
    void setZoom(double zoom, QPointF anchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void actualSize();
    FitMode fitMode() const { return m_fit; }
    void setFitMode(FitMode mode);
    // 100% shows the page at its physical size on this screen instead of
    // one point per logical pixel.
    void setPhysicalScale(bool on);
    double unitScale() const { return m_unit; }  // logical px per point at 100%
    // Re-reads page sizes from the document and relays out. Used when the
    // document's reported page size changes (e.g. the image scale preference).
    void refresh();
    // The document's pages changed (inserted, deleted, moved or rotated):
    // drops search hits and the selection, and relays out.
    void documentChanged();
    // Where a point on a page (in points) currently is in the viewport.
    QPointF mapFromPage(int page, QPointF pt) const { return pageTransform(page).map(pt); }
    // A fillable form field has the keyboard focus.
    bool formHasFocus() const;

    DisplayMode displayMode() const { return m_mode; }
    void setDisplayMode(DisplayMode mode);

    int rotation() const { return m_rotation; }
    void rotateBy(int degrees);

    int currentPage() const { return m_currentPage; }
    void goToPage(int page, double yPoints = -1);
    // Next/previous page, or spread in two-page mode.
    void stepPage(int direction);

    void setHits(const QVector<TextHit> &hits);
    void setCurrentHit(int index);  // scrolls it into view

    bool hasSelection() const { return !m_selection.text.isEmpty(); }
    QString selectedText() const { return m_selection.text; }
    void clearSelection();

    // --- Annotations (PDFs) ----------------------------------------------
    bool canAnnotate() const { return m_pdf && m_pdf->canAnnotate(); }
    Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    // Colours of new markup (highlights, underlines, strike-throughs, notes)
    // and of drawings, shapes and text, and the text box font size.
    void setAnnotationStyle(const QColor &markup, const QColor &pen, double textSize);
    // Turns the selected text into a highlight, underline or strike-through.
    bool markSelection(PdfDocument::Markup type);
    // The annotation clicked last (Select tool), if any.
    bool hasSelectedAnnotation() const { return m_annotation.isValid(); }
    bool selectedAnnotationHasText() const { return m_annotation.isNote || m_annotation.isTextBox; }
    bool selectedAnnotationIsLocked() const { return m_annotation.isLocked; }
    void deleteSelectedAnnotation();
    void editSelectedAnnotation();  // the text of a note or text box

signals:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void selectionChanged();
    void internalLinkActivated(int page);
    void toolChanged(Tool tool);  // the view went back to Select by itself (Escape)
    void annotationsChanged(int page);
    void annotationSelectionChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    bool viewportEvent(QEvent *) override;
    bool event(QEvent *) override;
    bool focusNextPrevChild(bool next) override;

private:
    void relayout();
    double scale() const { return m_zoom * m_unit; }  // logical px per point
    std::pair<int, int> shownPages() const;  // first, last laid out
    void updateUnitScale();
    void applyPendingPage();
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
    bool formKeyPress(QKeyEvent *e);
    // Annotations of a page, cached until they or the pages change.
    const QVector<PdfDocument::Annotation> &pageAnnotations(int page);
    PdfDocument::Annotation annotationAt(int page, QPointF pt);
    void selectAnnotation(const PdfDocument::Annotation &annotation);
    void finishToolGesture();
    bool askText(const QString &title, const QString &label, QString *text);

    DocumentPtr m_doc;
    PdfDocument *m_pdf = nullptr;  // m_doc, when it is a PDF
    QPointer<RenderService> m_renderer;
    double m_zoom = 1.0;
    double m_unit = 1.0;
    bool m_physical = false;
    FitMode m_fit = FitMode::Width;
    DisplayMode m_mode = DisplayMode::Continuous;
    int m_rotation = 0;
    int m_currentPage = 0;
    QVector<QRectF> m_rects;  // content px; null for pages not laid out
    QSizeF m_content;
    // goToPage() before the view has its size is applied once it does.
    int m_pendingPage = -1;
    double m_pendingY = -1;
    // Scrolling past either end of a page flips it (single/two pages).
    int m_flipAccum = 0;
    QElapsedTimer m_lastFlip;

    QVector<TextHit> m_hits;
    int m_currentHit = -1;

    // Mouse state
    QPointF m_pressPos;
    int m_pressPage = -1;
    QPointF m_pressPagePt;
    bool m_dragging = false;
    bool m_panning = false;
    bool m_formPress = false;  // the press went to a form field
    QPoint m_panStart;
    QPoint m_scrollStart;
    TextHit m_selection;

    // Annotation state
    Tool m_tool = Tool::Select;
    QColor m_markupColor{255, 212, 0};
    QColor m_penColor{220, 30, 30};
    double m_textSize = 12;
    bool m_toolDrag = false;  // drawing, or dragging out a shape or text box
    QPolygonF m_stroke;       // the drawing so far, page points
    QPointF m_toolEnd;
    PdfDocument::Annotation m_annotation;
    QHash<int, QVector<PdfDocument::Annotation>> m_annotCache;
    qint64 m_annotCacheRevision = -1;
};
