// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QAbstractListModel>
#include <QListView>
#include <QPointer>
#include <QTimer>

#include <functional>

class RenderService;

class ThumbnailModel : public QAbstractListModel {
    Q_OBJECT
public:
    ThumbnailModel(DocumentPtr doc, RenderService *renderer, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setThumbWidth(int w);
    // Page count or order changed.
    void reload();
    void setEditable(bool on) { m_editable = on; }
    bool isEditable() const { return m_editable; }
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    int thumbWidth() const { return m_width; }
    QSize thumbSize(int page) const;

private:
    DocumentPtr m_doc;
    QPointer<RenderService> m_renderer;
    int m_width = 120;
    bool m_editable = false;  // pages can be dragged
};

// Page thumbnails in a vertical strip. Thumbnails render lazily (only rows
// that are painted request a render) and stay in sync with the view.
//
// When editable (PDFs), pages can be selected together and dragged to a new
// place, and files or pages from another document can be dropped between
// thumbnails to insert them. The sidebar only reports these gestures; the
// document tab carries them out.
class ThumbnailSidebar : public QListView {
    Q_OBJECT
public:
    // Drag payload: pages of a document as a PDF of their own.
    static constexpr const char *kPagesMime = "application/x-xpreview-pages";

    ThumbnailSidebar(DocumentPtr doc, RenderService *renderer, QWidget *parent = nullptr);
    void setCurrentPage(int page);
    void setEditable(bool on);
    bool isEditable() const { return m_model->isEditable(); }
    // Page count or order changed; `select` become the selected pages.
    void reload(const QVector<int> &select);
    // Selected pages in ascending order.
    QVector<int> selectedPages() const;
    // Builds the PDF that a drag of these pages carries (set by the tab).
    std::function<QByteArray(const QVector<int> &)> pageExporter;
    int thumbnailWidth() const { return m_model->thumbWidth(); }
    bool layoutPending() const { return m_widthTimer.isActive(); }
    // Delay before a resize re-lays out the thumbnails (tests use 0).
    void setResizeDebounce(int ms) { m_widthTimer.setInterval(ms); }

signals:
    void pageActivated(int page);
    // Pages of this document dragged to sit before page `before`.
    void movePagesRequested(const QVector<int> &pages, int before);
    // A PDF (pages dragged from another document, or an Option/Alt-drag
    // copy) dropped before page `before`.
    void pdfDropped(const QByteArray &pdf, int before);
    void filesDropped(const QStringList &paths, int before);
    void deleteRequested();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    int targetThumbWidth() const;
    void applyThumbWidth();
    bool acceptsDrop(const QMimeData *mime) const;
    int dropIndex(QPoint pos) const;  // insertion point under the cursor
    void setDropIndex(int index);

    ThumbnailModel *m_model;
    QTimer m_widthTimer;
    bool m_syncing = false;
    int m_dropIndex = -1;
};
