// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QList>
#include <QMainWindow>

class DocumentTab;
class PreferencesDialog;
class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QTabWidget;
class QTimer;
class QToolBar;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    // Opens a file honouring the "Opening several files" preference: either a
    // new tab in this window, or a window of its own. A file already open
    // anywhere is raised instead.
    void openFile(const QString &path);
    // The frontmost window (active, else the most recently created), or null.
    static MainWindow *front();

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    DocumentTab *currentTab() const;
    void updateUi();
    void updateRecentMenu();
    void openDialog();
    void closeTab(int index);
    // Offers to save a modified document. False if the user cancelled.
    bool maybeSave(DocumentTab *tab);
    bool saveAs();
    bool saveTab(DocumentTab *tab);  // Save, or Save As for converted documents
    void updateTabText(DocumentTab *tab);
    void addRecent(const QString &path);
    void exportPdf();
    void print();
    void copy();
    void goToPageDialog();
    void setSidebarMode(int mode);
    void showPreferences();
    bool handleChromeMouse(QEvent *e);
    void updateTabBarStyle();
    void updateSearchStyle();
    void saveWindowGeometry();

    static QList<MainWindow *> s_windows;  // all open windows, newest last

    QTabWidget *m_tabs;
    QToolBar *m_toolBar = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_searchStatus;
    QLabel *m_pageLabel;
    QComboBox *m_zoomBox;
    QToolButton *m_sidebarButton;
    QMenu *m_recentMenu;
    PreferencesDialog *m_preferences = nullptr;
    // Saves the size and position shortly after the user stops moving or
    // resizing, so they survive a crash or forced quit, not just a close.
    QTimer *m_geometryTimer;

    QAction *m_open, *m_close, *m_save, *m_saveAs, *m_export, *m_print, *m_openDefault, *m_reveal;
    QAction *m_protect;
    QAction *m_undo, *m_redo, *m_insertFile, *m_insertBlank, *m_deletePages;
    QAction *m_rotPageLeft, *m_rotPageRight, *m_exportPages;
    QAction *m_copy, *m_find, *m_findNext, *m_findPrev;
    QAction *m_hideSidebar, *m_thumbs, *m_toc;
    QActionGroup *m_sidebarGroup;
    QAction *m_actual, *m_zoomIn, *m_zoomOut, *m_fitPage, *m_fitWidth;
    QAction *m_prevPage, *m_nextPage, *m_firstPage, *m_lastPage, *m_goTo;
    QAction *m_rotLeft, *m_rotRight, *m_inspector, *m_prefs;
    int m_sidebarMode = 1;
    // Window move by hand when the native one is refused (handleChromeMouse).
    bool m_chromeDrag = false;
    bool m_chromeDragRetry = false;
    QPoint m_chromeDragOffset;
};
