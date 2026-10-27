// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QMainWindow>

class DocumentTab;
class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QTabWidget;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    DocumentTab *currentTab() const;
    void updateUi();
    void updateRecentMenu();
    void openDialog();
    void closeTab(int index);
    void exportPdf();
    void print();
    void copy();
    void goToPageDialog();
    void setSidebarMode(int mode);

    QTabWidget *m_tabs;
    QLineEdit *m_search;
    QLabel *m_searchStatus;
    QLabel *m_pageLabel;
    QComboBox *m_zoomBox;
    QToolButton *m_sidebarButton;
    QMenu *m_recentMenu;

    QAction *m_open, *m_close, *m_export, *m_print, *m_openDefault, *m_reveal;
    QAction *m_copy, *m_find, *m_findNext, *m_findPrev;
    QAction *m_hideSidebar, *m_thumbs, *m_toc;
    QActionGroup *m_sidebarGroup;
    QAction *m_actual, *m_zoomIn, *m_zoomOut, *m_fitPage, *m_fitWidth;
    QAction *m_prevPage, *m_nextPage, *m_firstPage, *m_lastPage, *m_goTo;
    QAction *m_rotLeft, *m_rotRight, *m_inspector;
    int m_sidebarMode = 1;
};
