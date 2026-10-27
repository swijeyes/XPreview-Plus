// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QAbstractListModel>
#include <QListView>
#include <QPointer>
#include <QTimer>

class RenderService;

class ThumbnailModel : public QAbstractListModel {
    Q_OBJECT
public:
    ThumbnailModel(DocumentPtr doc, RenderService *renderer, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setThumbWidth(int w);
    int thumbWidth() const { return m_width; }
    QSize thumbSize(int page) const;

private:
    DocumentPtr m_doc;
    QPointer<RenderService> m_renderer;
    int m_width = 120;
};

// Page thumbnails in a vertical strip. Thumbnails render lazily (only rows
// that are painted request a render) and stay in sync with the view.
class ThumbnailSidebar : public QListView {
    Q_OBJECT
public:
    ThumbnailSidebar(DocumentPtr doc, RenderService *renderer, QWidget *parent = nullptr);
    void setCurrentPage(int page);
    int thumbnailWidth() const { return m_model->thumbWidth(); }
    bool layoutPending() const { return m_widthTimer.isActive(); }
    // Delay before a resize re-lays out the thumbnails (tests use 0).
    void setResizeDebounce(int ms) { m_widthTimer.setInterval(ms); }

signals:
    void pageActivated(int page);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    int targetThumbWidth() const;
    void applyThumbWidth();

    ThumbnailModel *m_model;
    QTimer m_widthTimer;
    bool m_syncing = false;
};
