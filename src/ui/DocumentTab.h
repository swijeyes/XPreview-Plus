// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/DocumentLoader.h"

#include <QFutureWatcher>
#include <QWidget>

class DocumentView;
class InfoBar;
class PdfDocument;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class RenderService;
class ThumbnailSidebar;

// A warning box that shows `text` literally (never as rich text).
void plainWarning(QWidget *parent, const QString &title, const QString &text);

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
    // The system opens files by extension, not content: a PDF named
    // "x.hta" or "x.command" would run as a program. A recognised document
    // is only handed over when its extension matches what it contains.
    bool canOpenInDefaultApp() const;

    void setSidebar(Sidebar mode);
    Sidebar sidebar() const { return m_sidebarMode; }

    void find(const QString &text);
    void findNext(int direction);
    int hitCount() const { return m_hits.size(); }
    int currentHit() const { return m_currentHit; }
    QString searchText() const { return m_searchText; }

    void showDetails();

    // --- Editing (PDFs) ---------------------------------------------------
    PdfDocument *pdf() const;  // the document, if it is a PDF
    // A PDF: can be saved, and filled in or protected as its permissions allow.
    bool isEditable() const { return pdf() != nullptr; }
    // Pages may be reordered, rotated, deleted or inserted.
    bool canEditPages() const;
    // Converted from another format: saving needs a new PDF file.
    bool needsSaveAs() const;
    // Shows the Password Protection dialog; true if the protection changed.
    bool editProtection();
    bool isModified() const;
    // Pages the page commands act on: the sidebar selection, else the
    // current page.
    QVector<int> targetPages() const;
    void deletePages();
    void rotatePages(int quarterTurns);
    void insertBlankPage();
    // Asks for files and inserts them after the current page.
    void insertFromFileDialog();
    // Inserts the files (PDFs, or anything that opens, converted to PDF).
    void insertFiles(const QStringList &paths, int before);
    void insertPdf(const QByteArray &pdf, int before);
    void movePages(const QVector<int> &pages, int before);
    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;
    void undo();
    void redo();
    // Saves to the file, or to `path` (Save As), which the tab then shows.
    bool save(const QString &path = {});
    // Writes the selected pages as a new PDF.
    void exportPages();

signals:
    void loaded();
    void stateChanged();  // page, zoom, selection or search changed
    void exportRequested();
    void modifiedChanged();
    void pathChanged();  // Save As gave the tab a new file

private:
    void load(const QByteArray &password);
    void onLoaded();
    void buildOutline();
    void showError(const QString &message);
    // After a page edit: refreshes thumbnails, view and outline.
    void pagesChanged(const QVector<int> &select);
    void showEditError(const QString &message);
    void onDocumentChange(int change, int page);
    void showPageMenu(const QPoint &pos);

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

    int m_passwordTries = 0;
    int m_searchRevision = 0;  // structure the running search started on
    QString m_searchText;
    QVector<TextHit> m_hits;
    int m_currentHit = -1;
};
