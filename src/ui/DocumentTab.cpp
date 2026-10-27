// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "DocumentTab.h"

#include "DocumentView.h"
#include "InfoBar.h"
#include "RenderService.h"
#include "ThumbnailSidebar.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

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

void DocumentTab::onLoaded()
{
    LoadResult r = m_loadWatcher.result();
    m_sniff = r.sniff;
    if (r.needsPassword) {
        bool ok = false;
        const QString pw = QInputDialog::getText(this, tr("Password Required"),
                                                 tr("“%1” is password protected. Enter the password:").arg(title()),
                                                 QLineEdit::Password, {}, &ok);
        if (ok && !pw.isEmpty()) {
            load(pw.toUtf8());
            return;
        }
        showError(r.error);
        return;
    }
    if (!r.document) {
        showError(r.error);
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
        if (flow) {
            m_infoBar->addAction(tr("Open in Default App"), [this] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
            });
            m_infoBar->addAction(tr("Export as PDF…"), [this] { emit exportRequested(); });
        }
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

    m_thumbs->setCurrentPage(0);
    setSidebar(m_sidebarMode);
    m_view->setFocus();
    emit loaded();
    emit stateChanged();
}

void DocumentTab::buildOutline()
{
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
                       .arg(title().toHtmlEscaped(), documentKindName(m_doc->kind()));
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
