// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/DocumentLoader.h"

#include <QFutureWatcher>
#include <QWidget>

class DocumentView;
class InfoBar;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class RenderService;
class ThumbnailSidebar;

// One open file: loads it off the GUI thread, then shows the sidebar
// (thumbnails or outline), the page view and an optional info bar.
class DocumentTab : public QWidget {
    Q_OBJECT
public:
    enum class Sidebar { Hidden, Thumbnails, Outline };

    explicit DocumentTab(const QString &path, QWidget *parent = nullptr);
    ~DocumentTab() override;

    QString path() const { return m_path; }
    QString title() const;
    bool isLoaded() const { return bool(m_doc); }
    DocumentPtr document() const { return m_doc; }
    DocumentView *view() const { return m_view; }
    const SniffResult &sniff() const { return m_sniff; }

    void setSidebar(Sidebar mode);
    Sidebar sidebar() const { return m_sidebarMode; }

    void find(const QString &text);
    void findNext(int direction);
    int hitCount() const { return m_hits.size(); }
    int currentHit() const { return m_currentHit; }
    QString searchText() const { return m_searchText; }

    void showDetails();

signals:
    void loaded();
    void stateChanged();  // page, zoom, selection or search changed
    void exportRequested();

private:
    void load(const QByteArray &password);
    void onLoaded();
    void buildOutline();
    void showError(const QString &message);

    QString m_path;
    DocumentPtr m_doc;
    SniffResult m_sniff;
    QFutureWatcher<LoadResult> m_loadWatcher;
    QFutureWatcher<QVector<TextHit>> m_searchWatcher;

    QStackedWidget *m_stack;
    QLabel *m_status;
    InfoBar *m_infoBar = nullptr;
    QSplitter *m_splitter = nullptr;
    QStackedWidget *m_sidebarStack = nullptr;
    ThumbnailSidebar *m_thumbs = nullptr;
    QTreeWidget *m_outline = nullptr;
    DocumentView *m_view = nullptr;
    RenderService *m_renderer = nullptr;
    Sidebar m_sidebarMode = Sidebar::Thumbnails;

    QString m_searchText;
    QVector<TextHit> m_hits;
    int m_currentHit = -1;
};
