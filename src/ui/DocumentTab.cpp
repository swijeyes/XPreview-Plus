// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "DocumentTab.h"

#include "Appearance.h"
#include "DocumentView.h"
#include "InfoBar.h"
#include "Preferences.h"
#include "ProtectionDialog.h"
#include "core/Exporter.h"
#include "core/Limits.h"
#include "image/ImageDocument.h"
#include "pdf/PdfDocument.h"
#include "RenderService.h"
#include "ThumbnailSidebar.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

void plainWarning(QWidget *parent, const QString &title, const QString &text)
{
    // File names and parser messages can contain markup; never render it.
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

namespace {

// Password prompt naming a file. The label is forced to plain text, so a file
// name cannot inject markup; the field is marked sensitive (no prediction or
// history).
QString askPassword(QWidget *parent, const QString &prompt, bool *ok)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Password Required"));
    dialog.setLabelText(prompt);
    dialog.setTextEchoMode(QLineEdit::Password);
    dialog.setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhHiddenText);
    for (QLabel *label : dialog.findChildren<QLabel *>()) label->setTextFormat(Qt::PlainText);
    *ok = dialog.exec() == QDialog::Accepted;
    return *ok ? dialog.textValue() : QString();
}

}  // namespace

DocumentTab::DocumentTab(const QString &path, QWidget *parent)
    : QWidget(parent), m_path(QFileInfo(path).absoluteFilePath())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_stack = new QStackedWidget(this);
    m_status = new QLabel(tr("Opening…"), this);
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    m_status->setMargin(40);
    m_stack->addWidget(m_status);
    layout->addWidget(m_stack);

    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &DocumentTab::onLoaded);
    connect(&m_searchWatcher, &QFutureWatcher<QVector<TextHit>>::finished, this, [this] {
        if (!m_view) return;
        // Pages changed while searching: the hits point at the old pages.
        if (pdf() && pdf()->structureRevision() != m_searchRevision) return;
        m_hits = m_searchWatcher.result();
        m_view->setHits(m_hits);
        // Start at the first hit on or after the current page.
        m_currentHit = m_hits.isEmpty() ? -1 : 0;
        for (int i = 0; i < m_hits.size(); ++i) {
            if (m_hits[i].page() >= m_view->currentPage()) {
                m_currentHit = i;
                break;
            }
        }
        m_view->setCurrentHit(m_currentHit);
        emit stateChanged();
    });
    load({});
}

DocumentTab::~DocumentTab()
{
    if (PdfDocument *p = pdf()) p->setChangeHandler({});
    if (m_view && m_doc && m_doc->kind() == DocumentKind::Pdf) Preferences::setLastPage(m_path, m_view->currentPage());
    // Let in-flight work finish; results are discarded.
    m_loadWatcher.waitForFinished();
    m_searchWatcher.waitForFinished();
}

QString DocumentTab::title() const { return QFileInfo(m_path).fileName(); }

void DocumentTab::load(const QByteArray &password)
{
    const QString path = m_path;
    m_loadWatcher.setFuture(QtConcurrent::run([path, password] { return loadDocument(path, password); }));
}

void DocumentTab::showError(const QString &message)
{
    m_status->setText(QStringLiteral("<b>%1</b><br><br>%2")
                          .arg(title().toHtmlEscaped(), message.toHtmlEscaped()));
}

bool DocumentTab::canOpenInDefaultApp() const
{
    if (m_loadWatcher.isRunning()) return false;  // not sniffed yet
    return m_sniff.format == SniffedFormat::Unknown || (m_sniff.knownExtension && !m_sniff.extensionMismatch);
}

void DocumentTab::onLoaded()
{
    LoadResult r = m_loadWatcher.result();
    m_sniff = r.sniff;
    if (r.needsPassword) {
        bool ok = false;
        const QString prompt = m_passwordTries == 0
                                   ? tr("“%1” is password protected. Enter the password:").arg(title())
                                   : tr("The password is incorrect. Enter the password for “%1”:").arg(title());
        const QString pw = askPassword(this, prompt, &ok);
        // A short pause after each wrong password slows guessing.
        if (ok && !pw.isEmpty() && m_passwordTries < 20) {
            const int tries = m_passwordTries++;
            QTimer::singleShot(tries == 0 ? 0 : qMin(2000, 250 * tries), this, [this, pw] { load(pw.toUtf8()); });
            return;
        }
        showError(r.error);
        emit stateChanged();  // sniffed: menu commands may change
        return;
    }
    if (!r.document) {
        showError(r.error);
        emit stateChanged();
        return;
    }
    m_doc = r.document;
    m_renderer = new RenderService(m_doc, this);

    auto *content = new QWidget(this);
    auto *vbox = new QVBoxLayout(content);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    const FidelityReport &report = m_doc->fidelityReport();
    if (report.isNotable() && m_doc->kind() != DocumentKind::Pdf) {
        m_infoBar = new InfoBar(content);
        const bool flow = m_doc->kind() == DocumentKind::Rtf || m_doc->kind() == DocumentKind::Odt;
        m_infoBar->setMessage(report.hasErrors()
                                  ? tr("Part of this document could not be read.")
                                  : flow ? tr("Simplified view: some formatting may differ from the original.")
                                         : report.lines().join(QLatin1Char('\n')));
        m_infoBar->addAction(tr("Details…"), [this] { showDetails(); });
        if (flow && canOpenInDefaultApp()) {
            m_infoBar->addAction(tr("Open in Default App"), [this] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
            });
        }
        if (flow) m_infoBar->addAction(tr("Export as PDF…"), [this] { emit exportRequested(); });
        vbox->addWidget(m_infoBar);
    } else if (PdfDocument *p = pdf(); p && (p->hasForm() || p->isRestricted() || p->isConverted())) {
        QStringList limits;
        if (!p->canPrint()) limits << tr("printing");
        if (!p->canCopy()) limits << tr("copying");
        if (!p->canEditPages() || !p->canFillForms()) limits << tr("changes");
        QStringList lines;
        if (p->isConverted())
            lines << tr("This %1 was converted to PDF for viewing. Use Save As to keep it as a PDF.").arg(p->kindName());
        if (!limits.isEmpty())
            lines << tr("The owner of this PDF does not allow %1.").arg(QLocale().createSeparatedList(limits));
        if (p->hasForm()) lines << tr("This PDF has form fields you can fill in. Click a field to type in it.");
        m_infoBar = new InfoBar(content);
        m_infoBar->setMessage(lines.join(QLatin1Char(' ')));
        vbox->addWidget(m_infoBar);
    }

    m_splitter = new QSplitter(Qt::Horizontal, content);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->setStyleSheet(QStringLiteral("QSplitter::handle { background: palette(mid); }"));
    m_sidebarStack = new QStackedWidget(m_splitter);
    m_thumbs = new ThumbnailSidebar(m_doc, m_renderer, m_sidebarStack);
    m_outline = new QTreeWidget(m_sidebarStack);
    m_outline->setHeaderHidden(true);
    m_outline->setFrameShape(QFrame::NoFrame);
    m_outline->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_sidebarStack->addWidget(m_thumbs);
    m_sidebarStack->addWidget(m_outline);
    m_sidebarStack->setMinimumWidth(110);
    // Source-list tint of the current theme.
    // Set on the views themselves: the splitter's style sheet stops palette
    // propagation from the stack.
    auto tintSidebar = [this] {
        for (QAbstractScrollArea *view : {static_cast<QAbstractScrollArea *>(m_thumbs),
                                          static_cast<QAbstractScrollArea *>(m_outline)}) {
            QPalette pal = view->palette();
            pal.setColor(QPalette::Base, Appearance::instance()->sidebarBackground());
            view->setPalette(pal);
            view->viewport()->setPalette(pal);
        }
    };
    tintSidebar();
    connect(Appearance::instance(), &Appearance::themeChanged, this, tintSidebar);

    m_view = new DocumentView(m_doc, m_renderer, m_splitter);
    m_splitter->addWidget(m_sidebarStack);
    m_splitter->addWidget(m_view);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({170, 800});
    vbox->addWidget(m_splitter, 1);
    m_stack->addWidget(content);
    m_stack->setCurrentWidget(content);

    buildOutline();

    connect(m_thumbs, &ThumbnailSidebar::pageActivated, m_view, [this](int p) { m_view->goToPage(p); });
    connect(m_view, &DocumentView::currentPageChanged, this, [this](int p) {
        m_thumbs->setCurrentPage(p);
        emit stateChanged();
    });
    connect(m_view, &DocumentView::zoomChanged, this, &DocumentTab::stateChanged);
    connect(m_view, &DocumentView::selectionChanged, this, &DocumentTab::stateChanged);
    connect(m_outline, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        m_view->goToPage(item->data(0, Qt::UserRole).toInt(), item->data(0, Qt::UserRole + 1).toDouble());
    });
    connect(m_outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        m_view->goToPage(item->data(0, Qt::UserRole).toInt(), item->data(0, Qt::UserRole + 1).toDouble());
    });

    if (m_doc->kind() == DocumentKind::Pdf) {
        Preferences *prefs = Preferences::instance();
        auto applyScale = [this] {
            m_view->setPhysicalScale(Preferences::instance()->pdfScale() == Preferences::PdfScale::RealSize);
        };
        applyScale();
        connect(prefs, &Preferences::pdfScaleChanged, this, applyScale);
        m_view->setDisplayMode(prefs->pdfDisplayMode());
        const int last = Preferences::lastPage(m_path);
        // After this event: the view gets its final size first.
        if (prefs->restoreLastPage() && last > 0 && last < m_doc->pageCount())
            QTimer::singleShot(0, m_view, [this, last] { m_view->goToPage(last); });
    }

    if (m_doc->kind() == DocumentKind::Image) {
        Preferences *prefs = Preferences::instance();
        auto *img = static_cast<ImageDocument *>(m_doc.get());
        auto applyImageScale = [this, img] {
            const bool real = Preferences::instance()->imageScale() == Preferences::ImageScale::RealSize;
            img->setPrintedSize(real);
            m_view->setPhysicalScale(real);
            m_view->refresh();
        };
        applyImageScale();
        connect(prefs, &Preferences::imageScaleChanged, this, applyImageScale);
    }

    if (PdfDocument *p = pdf()) {
        // Page editing in the sidebar, and live form-field updates.
        m_thumbs->setEditable(p->canEditPages());
        m_thumbs->pageExporter = [this](const QVector<int> &pages) {
            QString error;
            return pdf()->extractPages(pages, &error);
        };
        connect(m_thumbs, &ThumbnailSidebar::movePagesRequested, this, &DocumentTab::movePages);
        connect(m_thumbs, &ThumbnailSidebar::pdfDropped, this, &DocumentTab::insertPdf);
        connect(m_thumbs, &ThumbnailSidebar::filesDropped, this, &DocumentTab::insertFiles);
        connect(m_thumbs, &ThumbnailSidebar::deleteRequested, this, &DocumentTab::deletePages);
        connect(m_thumbs->selectionModel(), &QItemSelectionModel::selectionChanged, this, &DocumentTab::stateChanged);
        m_thumbs->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_thumbs, &QWidget::customContextMenuRequested, this, &DocumentTab::showPageMenu);
        QPointer<DocumentTab> self(this);
        // Called with PDFium's lock held, possibly off the GUI thread.
        p->setChangeHandler([self](PdfDocument::Change change, int page) {
            QMetaObject::invokeMethod(qApp, [self, change, page] {
                if (self) self->onDocumentChange(int(change), page);
            }, Qt::QueuedConnection);
        });
    }

    m_thumbs->setCurrentPage(m_view->currentPage());
    setSidebar(m_sidebarMode);
    m_view->setFocus();
    emit loaded();
    emit stateChanged();
}

void DocumentTab::buildOutline()
{
    m_outline->clear();
    std::function<void(QTreeWidgetItem *, const QList<OutlineItem> &)> add =
        [&](QTreeWidgetItem *parent, const QList<OutlineItem> &items) {
            for (const OutlineItem &o : items) {
                auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_outline);
                item->setText(0, o.title);
                // Tooltips render rich text; bookmark titles come from the file.
                item->setToolTip(0, QStringLiteral("<p>%1</p>").arg(o.title.toHtmlEscaped()));
                item->setData(0, Qt::UserRole, qMax(0, o.page));
                item->setData(0, Qt::UserRole + 1, o.y > 0 ? o.y : -1.0);
                add(item, o.children);
            }
        };
    add(nullptr, m_doc->outline());
    if (m_outline->topLevelItemCount() == 0) {
        auto *item = new QTreeWidgetItem(m_outline);
        item->setText(0, tr("No table of contents"));
        item->setFlags(Qt::NoItemFlags);
    }
    m_outline->expandToDepth(0);
}

void DocumentTab::setSidebar(Sidebar mode)
{
    m_sidebarMode = mode;
    if (!m_sidebarStack) return;
    m_sidebarStack->setVisible(mode != Sidebar::Hidden);
    if (mode == Sidebar::Thumbnails) m_sidebarStack->setCurrentWidget(m_thumbs);
    else if (mode == Sidebar::Outline) m_sidebarStack->setCurrentWidget(m_outline);
}

void DocumentTab::find(const QString &text)
{
    if (!m_doc) return;
    m_searchText = text;
    if (text.isEmpty()) {
        m_hits.clear();
        m_currentHit = -1;
        if (m_view) m_view->setHits({});
        emit stateChanged();
        return;
    }
    DocumentPtr doc = m_doc;
    if (pdf()) m_searchRevision = pdf()->structureRevision();
    m_searchWatcher.setFuture(QtConcurrent::run([doc, text] { return doc->search(text, false); }));
}

void DocumentTab::findNext(int direction)
{
    if (m_hits.isEmpty() || !m_view) return;
    m_currentHit = (m_currentHit + direction + m_hits.size()) % m_hits.size();
    m_view->setCurrentHit(m_currentHit);
    emit stateChanged();
}

void DocumentTab::showDetails()
{
    if (!m_doc) return;
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p><table>")
                       .arg(title().toHtmlEscaped(), m_doc->kindName());
    if (m_sniff.extensionMismatch)
        html += tr("<p><i>Note: the file extension does not match its contents.</i></p>");
    const auto meta = m_doc->metadata();
    for (auto it = meta.cbegin(); it != meta.cend(); ++it)
        html += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
                    .arg(it.key().toHtmlEscaped(), it.value().toHtmlEscaped());
    html += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
                .arg(tr("Pages"))
                .arg(m_doc->pageCount());
    html += QStringLiteral("</table>");
    const QStringList lines = m_doc->fidelityReport().lines();
    if (!lines.isEmpty()) {
        html += tr("<h4>Simplified in this view</h4><ul>");
        for (const QString &l : lines) html += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("Document Info"));
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.exec();
}

// --- Editing -------------------------------------------------------------------

PdfDocument *DocumentTab::pdf() const
{
    return m_doc && m_doc->kind() == DocumentKind::Pdf ? static_cast<PdfDocument *>(m_doc.get()) : nullptr;
}

bool DocumentTab::canEditPages() const { return pdf() && pdf()->canEditPages(); }

bool DocumentTab::needsSaveAs() const { return pdf() && pdf()->isConverted(); }

bool DocumentTab::editProtection()
{
    PdfDocument *p = pdf();
    if (!p) return false;
    ProtectionDialog dialog(*p, this);
    if (dialog.exec() != QDialog::Accepted) return false;
    QString error;
    if (!p->setProtection(dialog.protection(), &error)) {
        showEditError(error);
        return false;
    }
    emit modifiedChanged();
    emit stateChanged();
    return true;
}

bool DocumentTab::isModified() const { return pdf() && pdf()->isModified(); }

void DocumentTab::onDocumentChange(int change, int page)
{
    switch (PdfDocument::Change(change)) {
    case PdfDocument::Change::Appearance:
        m_renderer->invalidatePage(page);
        m_view->viewport()->update();
        m_thumbs->viewport()->update();
        break;
    case PdfDocument::Change::Modified: emit modifiedChanged(); break;
    case PdfDocument::Change::FormFocus: emit stateChanged(); break;
    }
}

QVector<int> DocumentTab::targetPages() const
{
    if (!m_view) return {};
    if (m_thumbs && m_sidebarMode == Sidebar::Thumbnails) {
        const QVector<int> sel = m_thumbs->selectedPages();
        if (!sel.isEmpty()) return sel;
    }
    return {m_view->currentPage()};
}

void DocumentTab::pagesChanged(const QVector<int> &select)
{
    m_renderer->reset();
    m_thumbs->reload(select);
    m_view->documentChanged();
    buildOutline();
    m_hits.clear();
    m_currentHit = -1;
    if (!m_searchText.isEmpty()) find(m_searchText);  // hits moved with their pages
    if (!select.isEmpty()) m_view->goToPage(select.first());
    emit stateChanged();
}

void DocumentTab::showEditError(const QString &message)
{
    plainWarning(this, tr("Could Not Change “%1”").arg(title()), message);
}

void DocumentTab::deletePages()
{
    PdfDocument *p = pdf();
    if (!p) return;
    const QVector<int> pages = targetPages();
    QString error;
    if (!p->deletePages(pages, &error)) {
        if (!error.isEmpty()) showEditError(error);
        return;
    }
    pagesChanged({qMin(pages.first(), p->pageCount() - 1)});
}

void DocumentTab::rotatePages(int quarterTurns)
{
    PdfDocument *p = pdf();
    const QVector<int> pages = targetPages();
    if (p && p->rotatePages(pages, quarterTurns)) pagesChanged(pages);
}

void DocumentTab::insertBlankPage()
{
    PdfDocument *p = pdf();
    if (!p || !m_view) return;
    const int current = m_view->currentPage();
    // Same size as the current page, after it.
    if (p->insertBlankPage(current + 1, p->pageSize(current))) pagesChanged({current + 1});
}

void DocumentTab::insertFromFileDialog()
{
    if (!pdf() || !m_view) return;
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Insert Pages from File"), s.value(QStringLiteral("lastDir"), QDir::homePath()).toString(),
        supportedFileFilter());
    if (files.isEmpty()) return;
    s.setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
    const QVector<int> target = targetPages();
    insertFiles(files, (target.isEmpty() ? m_view->currentPage() : target.last()) + 1);
}

void DocumentTab::insertFiles(const QStringList &paths, int before)
{
    PdfDocument *p = pdf();
    if (!p) return;
    QVector<int> inserted;
    QStringList failed;
    int at = before;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        const QString name = info.fileName();
        // Regular files only (a FIFO or device would block), and bounded.
        if (!info.isFile()) {
            failed << QStringLiteral("%1: %2").arg(name, tr("not a file"));
            continue;
        }
        if (info.size() > Limits::kMaxInsertPdfBytes) {
            failed << QStringLiteral("%1: %2").arg(name, tr("too large to insert"));
            continue;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        LoadResult r = loadDocument(path);
        QApplication::restoreOverrideCursor();
        QByteArray password;
        const auto wipePassword = qScopeGuard([&password] { password.fill('\0'); });
        if (r.needsPassword) {
            bool ok = false;
            const QString pw = askPassword(this, tr("“%1” is password protected. Enter the password:").arg(name), &ok);
            if (!ok || pw.isEmpty()) continue;
            password = pw.toUtf8();
            r = loadDocument(path, password);
        }
        if (!r.document) {
            failed << QStringLiteral("%1: %2").arg(name, r.error);
            continue;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        // PDFs go in as they are; anything else (including PostScript, already
        // converted in memory) is converted first, as by Export as PDF.
        QByteArray data;
        QString error;
        if (r.document->kind() == DocumentKind::Pdf && !static_cast<PdfDocument *>(r.document.get())->isConverted()) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) data = f.read(Limits::kMaxInsertPdfBytes + 1);
        } else {
            QTemporaryDir dir;
            const QString tmp = dir.filePath(QStringLiteral("insert.pdf"));
            if (dir.isValid() && Exporter::exportPdf(*r.document, tmp, 0, &error)) {
                QFile f(tmp);
                if (f.open(QIODevice::ReadOnly)) data = f.readAll();
            }
        }
        const int n = data.isEmpty() ? 0 : p->insertPdf(data, password, at, &error);
        QApplication::restoreOverrideCursor();
        if (n == 0) {
            failed << QStringLiteral("%1: %2").arg(name, error.isEmpty() ? tr("could not be read") : error);
            continue;
        }
        for (int k = 0; k < n; ++k) inserted << at + k;
        at += n;
    }
    if (!inserted.isEmpty()) pagesChanged(inserted);
    if (!failed.isEmpty()) showEditError(tr("Some files could not be inserted:\n\n%1").arg(failed.join(QLatin1Char('\n'))));
}

void DocumentTab::insertPdf(const QByteArray &data, int before)
{
    PdfDocument *p = pdf();
    if (!p) return;
    QString error;
    const int n = p->insertPdf(data, {}, before, &error);
    if (n == 0) return showEditError(error);
    QVector<int> inserted;
    for (int k = 0; k < n; ++k) inserted << before + k;
    pagesChanged(inserted);
}

void DocumentTab::movePages(const QVector<int> &pages, int before)
{
    PdfDocument *p = pdf();
    if (!p) return;
    const int dest = p->movePages(pages, before);
    if (dest < 0) return;
    QVector<int> moved;
    for (int k = 0; k < pages.size(); ++k) moved << dest + k;
    pagesChanged(moved);
}

bool DocumentTab::canUndo() const { return pdf() && pdf()->canUndo(); }
bool DocumentTab::canRedo() const { return pdf() && pdf()->canRedo(); }
QString DocumentTab::undoText() const { return canUndo() ? tr("Undo %1").arg(pdf()->undoLabel()) : tr("Undo"); }
QString DocumentTab::redoText() const { return canRedo() ? tr("Redo %1").arg(pdf()->redoLabel()) : tr("Redo"); }

void DocumentTab::undo()
{
    if (!canUndo() || !m_view) return;
    const int page = m_view->currentPage();
    if (pdf()->undo()) pagesChanged({qMin(page, pdf()->pageCount() - 1)});
}

void DocumentTab::redo()
{
    if (!canRedo() || !m_view) return;
    const int page = m_view->currentPage();
    if (pdf()->redo()) pagesChanged({qMin(page, pdf()->pageCount() - 1)});
}

bool DocumentTab::save(const QString &path)
{
    PdfDocument *p = pdf();
    if (!p) return false;
    const QString target = path.isEmpty() ? m_path : QFileInfo(path).absoluteFilePath();
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = p->save(target, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        plainWarning(this, tr("Save Failed"), error);
        return false;
    }
    if (target != m_path) {
        m_path = target;
        emit pathChanged();
    }
    emit modifiedChanged();
    emit stateChanged();
    return true;
}

void DocumentTab::exportPages()
{
    PdfDocument *p = pdf();
    if (!p) return;
    const QVector<int> pages = targetPages();
    if (pages.isEmpty()) return;
    const QFileInfo fi(m_path);
    const QString suggested = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
                              (pages.size() == 1 ? tr(" (page %1)").arg(pages.first() + 1) : tr(" (pages)")) +
                              QStringLiteral(".pdf");
    QString path = QFileDialog::getSaveFileName(this, tr("Export Pages as PDF"), suggested, tr("PDF document (*.pdf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) path += QStringLiteral(".pdf");
    QString error;
    const QByteArray data = p->extractPages(pages, &error);
    QSaveFile out(path);
    if (data.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
        plainWarning(this, tr("Export Failed"), error.isEmpty() ? tr("Could not write %1").arg(path) : error);
}

void DocumentTab::showPageMenu(const QPoint &pos)
{
    PdfDocument *p = pdf();
    if (!p) return;
    // Right-clicking an unselected thumbnail selects it first.
    const QModelIndex index = m_thumbs->indexAt(pos);
    if (index.isValid() && !m_thumbs->selectionModel()->isSelected(index)) m_thumbs->setCurrentIndex(index);
    const int n = targetPages().size();
    QMenu menu(this);
    menu.addAction(n > 1 ? tr("Rotate %1 Pages Left").arg(n) : tr("Rotate Page Left"), this, [this] { rotatePages(-1); });
    menu.addAction(n > 1 ? tr("Rotate %1 Pages Right").arg(n) : tr("Rotate Page Right"), this, [this] { rotatePages(1); });
    menu.addSeparator();
    menu.addAction(tr("Insert Blank Page"), this, [this] { insertBlankPage(); });
    menu.addAction(tr("Insert Pages from File…"), this, [this] { insertFromFileDialog(); });
    menu.addSeparator();
    menu.addAction(n > 1 ? tr("Export %1 Pages as PDF…").arg(n) : tr("Export Page as PDF…"), this, [this] { exportPages(); });
    menu.addSeparator();
    QAction *del = menu.addAction(n > 1 ? tr("Delete %1 Pages").arg(n) : tr("Delete Page"), this, [this] { deletePages(); });
    del->setEnabled(n < p->pageCount());
    menu.exec(m_thumbs->viewport()->mapToGlobal(pos));
}
