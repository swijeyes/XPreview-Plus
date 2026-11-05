// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "MainWindow.h"

#include "Appearance.h"
#include "AppStyle.h"
#include "DocumentTab.h"
#include "DocumentView.h"
#include "Preferences.h"
#include "core/DocumentLoader.h"
#include "core/Exporter.h"
#include "image/ImageDocument.h"
#include "PreferencesDialog.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>

#include <utility>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QProgressDialog>
#include <QStyleOptionToolButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent>

namespace {

// Theme icon (SF Symbols on macOS, freedesktop on Linux) with a drawn
// fallback so the toolbar never shows empty buttons.
QIcon themedIcon(QIcon::ThemeIcon theme, const std::function<void(QPainter &, const QRectF &)> &draw)
{
    QIcon icon = QIcon::fromTheme(theme);
    if (!icon.isNull()) return icon;
    QIcon drawn;
    for (int size : {16, 32, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QApplication::palette().color(QPalette::ButtonText), size / 14.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        draw(p, QRectF(size * 0.12, size * 0.12, size * 0.76, size * 0.76));
        drawn.addPixmap(pm);
    }
    return drawn;
}

QIcon sidebarIcon()
{
    QIcon icon;
    for (int size : {16, 32, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor c = QApplication::palette().color(QPalette::ButtonText);
        p.setPen(QPen(c, size / 16.0));
        const QRectF r(size * 0.1, size * 0.2, size * 0.8, size * 0.6);
        p.drawRoundedRect(r, size * 0.08, size * 0.08);
        p.drawLine(QPointF(r.left() + r.width() * 0.32, r.top()), QPointF(r.left() + r.width() * 0.32, r.bottom()));
        icon.addPixmap(pm);
    }
    return icon;
}

void drawMagnifier(QPainter &p, const QRectF &r, bool plus)
{
    const QRectF lens(r.left(), r.top(), r.width() * 0.7, r.height() * 0.7);
    p.drawEllipse(lens);
    p.drawLine(lens.center() + QPointF(lens.width() * 0.36, lens.height() * 0.36), r.bottomRight());
    const QPointF c = lens.center();
    const double h = lens.width() * 0.22;
    p.drawLine(c - QPointF(h, 0), c + QPointF(h, 0));
    if (plus) p.drawLine(c - QPointF(0, h), c + QPointF(0, h));
}

void drawRotate(QPainter &p, const QRectF &r, bool left)
{
    QPainterPath path;
    const QRectF arc = r.adjusted(r.width() * 0.1, r.height() * 0.15, -r.width() * 0.1, -r.height() * 0.05);
    path.arcMoveTo(arc, left ? 60 : 120);
    path.arcTo(arc, left ? 60 : 120, left ? 260 : -260);
    p.drawPath(path);
    const QPointF tip = left ? QPointF(arc.center().x() + arc.width() * 0.25, arc.top() + arc.height() * 0.07)
                             : QPointF(arc.center().x() - arc.width() * 0.25, arc.top() + arc.height() * 0.07);
    const double s = r.width() * 0.2;
    p.drawLine(tip, tip + QPointF(left ? -s : s, -s * 0.4));
    p.drawLine(tip, tip + QPointF(left ? -s * 0.3 : s * 0.3, s * 0.9));
}

// Classic "textured rounded" toolbar button. Buttons placed side by
// side with First/Middle/Last join into one segmented control.
class TexturedButton : public QToolButton {
public:
    enum Segment { Only, First, Middle, Last };

    TexturedButton(Segment segment, QAction *action, QWidget *parent) : QToolButton(parent), m_segment(segment)
    {
        if (action) setDefaultAction(action);
        setFocusPolicy(Qt::NoFocus);
        setIconSize(QSize(16, 16));
        setFixedSize(32, 24);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool active = isActiveWindow();
        const bool down = isDown() || isChecked();
        const double radius = 4;
        // Bottom pixel is left for the embossed highlight.
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -1.5);
        // Inner ends extend past the button so only outer corners are round.
        QRectF shape = r;
        if (m_segment == First || m_segment == Middle) shape.setRight(r.right() + radius + 2);
        if (m_segment == Middle || m_segment == Last) shape.setLeft(r.left() - radius - 2);
        p.setClipRect(rect());

        p.setPen(QColor(255, 255, 255, active ? 140 : 90));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(shape.translated(0, 1), radius, radius);

        QLinearGradient g(0, r.top(), 0, r.bottom());
        if (down) {
            g.setColorAt(0, QColor(0x8c, 0x8c, 0x8c));
            g.setColorAt(1, QColor(0xb4, 0xb4, 0xb4));
        } else if (active) {
            g.setColorAt(0, QColor(0xfd, 0xfd, 0xfd));
            g.setColorAt(1, QColor(0xd3, 0xd3, 0xd3));
        } else {
            g.setColorAt(0, QColor(0xf8, 0xf8, 0xf8));
            g.setColorAt(1, QColor(0xe6, 0xe6, 0xe6));
        }
        const QColor outline = active ? QColor(0x5e, 0x5e, 0x5e) : QColor(0x9c, 0x9c, 0x9c);
        p.setPen(outline);
        p.setBrush(g);
        p.drawRoundedRect(shape, radius, radius);
        p.setRenderHint(QPainter::Antialiasing, false);
        if (m_segment == First || m_segment == Middle) p.drawLine(QPointF(width() - 1, r.top()), QPointF(width() - 1, r.bottom()));

        QRect content = rect().adjusted(0, 0, 0, -1);
        if (popupMode() == MenuButtonPopup && menu()) {
            QStyleOptionToolButton opt;
            initStyleOption(&opt);
            const QRect menuRect = style()->subControlRect(QStyle::CC_ToolButton, &opt, QStyle::SC_ToolButtonMenu, this);
            content.setRight(menuRect.left());
            const QPointF c(menuRect.center().x() + 0.5, r.center().y() + 1);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(isEnabled() ? QColor(0x30, 0x30, 0x30) : QColor(0x9a, 0x9a, 0x9a));
            p.drawPolygon(QPolygonF{c + QPointF(-3, -1.5), c + QPointF(3, -1.5), c + QPointF(0, 2)});
        }
        const QSize s = iconSize();
        const QRect iconRect(content.center().x() - s.width() / 2 + 1, content.center().y() - s.height() / 2 + 1,
                             s.width(), s.height());
        icon().paint(&p, iconRect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
                     isChecked() ? QIcon::On : QIcon::Off);
    }

private:
    Segment m_segment;
};

// Classic toolbar item: controls with a small caption underneath.
QWidget *toolGroup(const QString &caption, const QList<QWidget *> &controls, QWidget *parent)
{
    auto *group = new QWidget(parent);
    group->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *column = new QVBoxLayout(group);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    auto *strip = new QWidget(group);
    strip->setFixedHeight(24);
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (QWidget *c : controls) row->addWidget(c, 0, Qt::AlignVCenter);
    column->addWidget(strip, 0, Qt::AlignHCenter);
    auto *label = new QLabel(caption, group);
    label->setAlignment(Qt::AlignHCenter);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() - 2);
    label->setFont(f);
    column->addWidget(label);
    return group;
}

QStringList recentFiles()
{
    return QSettings().value(QStringLiteral("recentFiles")).toStringList();
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setAcceptDrops(true);
#ifdef Q_OS_MACOS
    // Classic unified window: the gray gradient runs up under a
    // transparent title bar (see paintEvent and AppStyle's CE_ToolBar).
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setWindowFlag(Qt::NoTitleBarBackgroundHint);
#endif
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setTabBarAutoHide(true);
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->tabBar()->setExpanding(true);
    updateTabBarStyle();
    connect(Appearance::instance(), &Appearance::themeChanged, this, &MainWindow::updateTabBarStyle);
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (DocumentTab *t = currentTab()) {
            t->setSidebar(DocumentTab::Sidebar(m_sidebarMode));
            m_search->setText(t->searchText());
        }
        updateUi();
    });

    m_sidebarMode = QSettings().value(QStringLiteral("sidebarMode"), 1).toInt();
    createActions();
    createMenus();
    createToolBar();

    m_pageLabel = new QLabel(this);
    m_pageLabel->setTextFormat(Qt::PlainText);
    m_zoomBox = new QComboBox(this);
    m_zoomBox->setEditable(true);
    m_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    m_zoomBox->addItems({tr("Fit Width"), tr("Fit Page"), QStringLiteral("50%"), QStringLiteral("75%"),
                         QStringLiteral("100%"), QStringLiteral("125%"), QStringLiteral("150%"),
                         QStringLiteral("200%"), QStringLiteral("400%"), QStringLiteral("800%")});
    m_zoomBox->setMinimumContentsLength(8);
    connect(m_zoomBox, &QComboBox::textActivated, this, [this](const QString &text) {
        DocumentTab *t = currentTab();
        if (!t || !t->view()) return;
        if (text == tr("Fit Width")) t->view()->setFitMode(DocumentView::FitMode::Width);
        else if (text == tr("Fit Page")) t->view()->setFitMode(DocumentView::FitMode::Page);
        else {
            bool ok = false;
            const double pct = QString(text).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
            if (ok && pct > 0) t->view()->setZoom(pct / 100.0);
        }
        t->view()->setFocus();
    });
    statusBar()->addWidget(m_pageLabel, 1);
    statusBar()->addPermanentWidget(m_zoomBox);
    statusBar()->setSizeGripEnabled(false);

    QSettings s;
    if (!restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray())) resize(1100, 820);
    updateUi();
    s_windows.append(this);
}

QList<MainWindow *> MainWindow::s_windows;

MainWindow::~MainWindow() { s_windows.removeAll(this); }

MainWindow *MainWindow::front()
{
    if (auto *w = qobject_cast<MainWindow *>(QApplication::activeWindow())) return w;
    return s_windows.isEmpty() ? nullptr : s_windows.last();
}

void MainWindow::updateTabBarStyle()
{
    Appearance *a = Appearance::instance();
    if (!a->isFlat()) {
        // Classic dark-gray tabs.
        m_tabs->tabBar()->setStyleSheet(QStringLiteral(
            "QTabBar { background: #a3a3a3; }"
            "QTabBar::tab { color: #262626; padding: 3px 20px; min-width: 80px;"
            "  border: 1px solid #5f5f5f; border-left: none; border-top-color: #6e6e6e;"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #b3b3b3, stop:1 #9e9e9e); }"
            "QTabBar::tab:first { border-left: 1px solid #5f5f5f; }"
            "QTabBar::tab:!selected:hover {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #bdbdbd, stop:1 #a8a8a8); }"
            "QTabBar::tab:selected { color: black; border-top: none;"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d8d8d8, stop:1 #c4c4c4); }"));
        return;
    }
    // Flat tabs: the selected one lifts to the surface colour with an accent
    // underline; the rest sit in the window chrome.
    const bool dark = a->isDark();
    const QString chrome = a->windowChrome().name();
    const QString border = a->chromeBorder().name();
    const QString text = dark ? QStringLiteral("#e5e5ea") : QStringLiteral("#1d1d1f");
    const QString dim = dark ? QStringLiteral("#8e8e93") : QStringLiteral("#86868b");
    const QString surface = dark ? QStringLiteral("#1c1c1e") : QStringLiteral("#ffffff");
    const QString accent = QStringLiteral("#0a84ff");
    m_tabs->tabBar()->setStyleSheet(
        QStringLiteral("QTabBar { background: %1; }"
                       "QTabBar::tab { color: %5; padding: 5px 20px; min-width: 80px;"
                       "  border: 1px solid %2; border-left: none; }"
                       "QTabBar::tab:first { border-left: 1px solid %2; }"
                       "QTabBar::tab:!selected { color: %6; background: %1; }"
                       "QTabBar::tab:selected { color: %3; background: %4; border-bottom: 2px solid %7; }")
            .arg(chrome, border, text, surface, text, dim, accent));
}

void MainWindow::createActions()
{
    auto make = [this](const QString &text, const QKeySequence &key, auto slot) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    auto view = [this] { return currentTab() ? currentTab()->view() : nullptr; };

    m_open = make(tr("Open…"), QKeySequence::Open, [this] { openDialog(); });
    m_close = make(tr("Close Tab"), QKeySequence::Close, [this] { closeTab(m_tabs->currentIndex()); });
    m_export = make(tr("Export as PDF…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { exportPdf(); });
    m_print = make(tr("Print…"), QKeySequence::Print, [this] { print(); });
    m_openDefault = make(tr("Open in Default App"), {}, [this] {
        if (DocumentTab *t = currentTab()) QDesktopServices::openUrl(QUrl::fromLocalFile(t->path()));
    });
#ifdef Q_OS_MACOS
    m_reveal = make(tr("Reveal in Finder"), {}, [this] {
        if (DocumentTab *t = currentTab()) QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), t->path()});
    });
#else
    m_reveal = make(tr("Show in Folder"), {}, [this] {
        if (DocumentTab *t = currentTab())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(t->path()).absolutePath()));
    });
#endif

    m_copy = make(tr("Copy"), QKeySequence::Copy, [this] { copy(); });
    m_find = make(tr("Find…"), QKeySequence::Find, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    m_findNext = make(tr("Find Next"), QKeySequence::FindNext, [this] {
        if (DocumentTab *t = currentTab()) t->findNext(1);
    });
    m_findPrev = make(tr("Find Previous"), QKeySequence::FindPrevious, [this] {
        if (DocumentTab *t = currentTab()) t->findNext(-1);
    });

    m_sidebarGroup = new QActionGroup(this);
    m_hideSidebar = make(tr("Hide Sidebar"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_1), [this] { setSidebarMode(0); });
    m_thumbs = make(tr("Thumbnails"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_2), [this] { setSidebarMode(1); });
    m_toc = make(tr("Table of Contents"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_3), [this] { setSidebarMode(2); });
    for (QAction *a : {m_hideSidebar, m_thumbs, m_toc}) {
        a->setCheckable(true);
        m_sidebarGroup->addAction(a);
    }

    m_actual = make(tr("Actual Size"), QKeySequence(Qt::CTRL | Qt::Key_0), [view] { if (auto *v = view()) v->actualSize(); });
    m_zoomIn = make(tr("Zoom In"), QKeySequence::ZoomIn, [view] { if (auto *v = view()) v->zoomIn(); });
    m_zoomOut = make(tr("Zoom Out"), QKeySequence::ZoomOut, [view] { if (auto *v = view()) v->zoomOut(); });
    m_fitPage = make(tr("Zoom to Fit"), QKeySequence(Qt::CTRL | Qt::Key_9),
                     [view] { if (auto *v = view()) v->setFitMode(DocumentView::FitMode::Page); });
    m_fitWidth = make(tr("Fit Width"), QKeySequence(Qt::CTRL | Qt::Key_8),
                      [view] { if (auto *v = view()) v->setFitMode(DocumentView::FitMode::Width); });
    m_zoomIn->setIcon(themedIcon(QIcon::ThemeIcon::ZoomIn, [](QPainter &p, const QRectF &r) { drawMagnifier(p, r, true); }));
    m_zoomOut->setIcon(themedIcon(QIcon::ThemeIcon::ZoomOut, [](QPainter &p, const QRectF &r) { drawMagnifier(p, r, false); }));
    // Cmd+= is what people press for zoom in on US keyboards.
    m_zoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});

    m_prevPage = make(tr("Previous Page"), QKeySequence(Qt::ALT | Qt::CTRL | Qt::Key_Up),
                      [view] { if (auto *v = view()) v->stepPage(-1); });
    m_nextPage = make(tr("Next Page"), QKeySequence(Qt::ALT | Qt::CTRL | Qt::Key_Down),
                      [view] { if (auto *v = view()) v->stepPage(1); });
    m_firstPage = make(tr("First Page"), QKeySequence(Qt::CTRL | Qt::Key_Up), [view] { if (auto *v = view()) v->goToPage(0); });
    m_lastPage = make(tr("Last Page"), QKeySequence(Qt::CTRL | Qt::Key_Down), [this, view] {
        if (auto *v = view()) v->goToPage(currentTab()->document()->pageCount() - 1);
    });
    m_goTo = make(tr("Go to Page…"), QKeySequence(Qt::ALT | Qt::CTRL | Qt::Key_G), [this] { goToPageDialog(); });

    m_rotLeft = make(tr("Rotate Left"), QKeySequence(Qt::CTRL | Qt::Key_L), [view] { if (auto *v = view()) v->rotateBy(-90); });
    m_rotRight = make(tr("Rotate Right"), QKeySequence(Qt::CTRL | Qt::Key_R), [view] { if (auto *v = view()) v->rotateBy(90); });
    m_rotLeft->setIcon(themedIcon(QIcon::ThemeIcon::ObjectRotateLeft, [](QPainter &p, const QRectF &r) { drawRotate(p, r, true); }));
    m_rotRight->setIcon(themedIcon(QIcon::ThemeIcon::ObjectRotateRight, [](QPainter &p, const QRectF &r) { drawRotate(p, r, false); }));
    m_inspector = make(tr("Show Inspector"), QKeySequence(Qt::CTRL | Qt::Key_I), [this] {
        if (DocumentTab *t = currentTab()) t->showDetails();
    });
    m_prefs = make(tr("Preferences…"), QKeySequence::Preferences, [this] { showPreferences(); });
    m_prefs->setMenuRole(QAction::PreferencesRole);
    m_export->setIcon(themedIcon(QIcon::ThemeIcon::DocumentSaveAs, [](QPainter &p, const QRectF &r) {
        p.drawRect(r.adjusted(r.width() * 0.15, 0, -r.width() * 0.15, 0));
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.2), QPointF(r.center().x(), r.bottom() - r.height() * 0.2));
    }));
}

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_open);
    m_recentMenu = file->addMenu(tr("Open Recent"));
    file->addSeparator();
    file->addAction(m_close);
    file->addSeparator();
    file->addAction(m_export);
    file->addAction(m_print);
    file->addSeparator();
    file->addAction(m_openDefault);
    file->addAction(m_reveal);
    auto *quit = file->addAction(tr("Quit"), QKeySequence::Quit, qApp, &QApplication::closeAllWindows);
    quit->setMenuRole(QAction::QuitRole);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_copy);
    edit->addSeparator();
    edit->addAction(m_find);
    edit->addAction(m_findNext);
    edit->addAction(m_findPrev);
    edit->addSeparator();
    edit->addAction(m_prefs);  // moves to the application menu on macOS

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_hideSidebar);
    view->addAction(m_thumbs);
    view->addAction(m_toc);
    view->addSeparator();
    view->addAction(m_actual);
    view->addAction(m_zoomIn);
    view->addAction(m_zoomOut);
    view->addAction(m_fitPage);
    view->addAction(m_fitWidth);

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    go->addAction(m_prevPage);
    go->addAction(m_nextPage);
    go->addSeparator();
    go->addAction(m_firstPage);
    go->addAction(m_lastPage);
    go->addSeparator();
    go->addAction(m_goTo);

    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(m_rotLeft);
    tools->addAction(m_rotRight);
    tools->addSeparator();
    tools->addAction(m_inspector);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    const QString name = QApplication::applicationDisplayName();
    auto *about = help->addAction(tr("About %1").arg(name), this, [this, name] {
        // "Appropriate Legal Notices" required by the GNU AGPL, section 0/5(d).
        QMessageBox::about(
            this, tr("About %1").arg(name),
            tr("<h3>%1 %2</h3><p>A fast viewer for PDF, images, RTF and OpenDocument text.</p>"
               "<p>%4</p>"
               "<p>This program is free software: you can redistribute it and/or modify it under "
               "the terms of the GNU Affero General Public License as published by the Free "
               "Software Foundation, either version 3 of the License, or (at your option) any "
               "later version.</p><p>This program comes with <b>ABSOLUTELY NO WARRANTY</b>; see "
               "the license for details.</p><p>Source code: <a href=\"%3\">%3</a></p>"
               "<p>Includes Qt (LGPLv3), PDFium (BSD-3-Clause/Apache-2.0), miniz (MIT), the "
               "Arimo, Tinos and Cousine fonts (Apache-2.0) and the Carlito and Caladea fonts "
               "(SIL OFL 1.1). See THIRD_PARTY_NOTICES.md.</p>")
                .arg(name.toHtmlEscaped(), QApplication::applicationVersion(),
                     QStringLiteral(XPREVIEW_SOURCE_URL),
                     QStringLiteral(XPREVIEW_COPYRIGHT).replace(QStringLiteral("(C)"), QStringLiteral("\u00a9"))));
    });
    about->setMenuRole(QAction::AboutRole);
    updateRecentMenu();
}

void MainWindow::createToolBar()
{
    QToolBar *tb = addToolBar(tr("Toolbar"));
    m_toolBar = tb;
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setMovable(false);
    auto gap = [this, tb](int w) {
        auto *g = new QWidget(this);
        g->setFixedWidth(w);
        tb->addWidget(g);
    };
    gap(6);  // QToolBar ignores contents margins

    m_sidebarButton = new TexturedButton(TexturedButton::Only, nullptr, this);
    m_sidebarButton->setIcon(sidebarIcon());
    m_sidebarButton->setToolTip(tr("Sidebar"));
    m_sidebarButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_sidebarButton->setFixedWidth(44);
    auto *menu = new QMenu(m_sidebarButton);
    menu->addAction(m_hideSidebar);
    menu->addAction(m_thumbs);
    menu->addAction(m_toc);
    m_sidebarButton->setMenu(menu);
    connect(m_sidebarButton, &QToolButton::clicked, this, [this] { setSidebarMode(m_sidebarMode == 0 ? 1 : 0); });
    tb->addWidget(toolGroup(tr("Sidebar"), {m_sidebarButton}, this));
    gap(14);
    tb->addWidget(toolGroup(tr("Zoom"), {new TexturedButton(TexturedButton::First, m_zoomOut, this),
                                         new TexturedButton(TexturedButton::Last, m_zoomIn, this)}, this));
    gap(14);
    tb->addWidget(toolGroup(tr("Rotate"), {new TexturedButton(TexturedButton::First, m_rotLeft, this),
                                           new TexturedButton(TexturedButton::Last, m_rotRight, this)}, this));

    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    tb->addWidget(toolGroup(tr("Export"), {new TexturedButton(TexturedButton::Only, m_export, this)}, this));
    gap(8);
    m_searchStatus = new QLabel(this);
    m_searchStatus->setMinimumWidth(70);
    m_searchStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tb->addWidget(m_searchStatus);
    gap(6);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedSize(200, 22);
    m_search->setAttribute(Qt::WA_MacShowFocusRect, false);
    // Rounded search field, as in classic toolbars.
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit { background: white; border: 1px solid #6f6f6f; border-radius: 10px; padding: 0 6px; }"
        "QLineEdit:focus { border: 2px solid palette(highlight); padding: 0 5px; }"
        "QLineEdit:disabled { background: #ececec; border-color: #9a9a9a; }"));
    tb->addWidget(toolGroup(tr("Search"), {m_search}, this));
    gap(4);
    // Empty toolbar space and captions move the window (handleChromeMouse).
    tb->installEventFilter(this);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        DocumentTab *t = currentTab();
        if (!t) return;
        if (t->searchText() != m_search->text()) t->find(m_search->text());
        else t->findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier ? -1 : 1);
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        // Clearing the field clears highlights immediately; typing searches
        // on Return so large documents are not searched per keystroke.
        if (text.isEmpty())
            if (DocumentTab *t = currentTab()) t->find({});
    });
}

DocumentTab *MainWindow::currentTab() const
{
    return qobject_cast<DocumentTab *>(m_tabs->currentWidget());
}

void MainWindow::setSidebarMode(int mode)
{
    m_sidebarMode = mode;
    QSettings().setValue(QStringLiteral("sidebarMode"), mode);
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *t = qobject_cast<DocumentTab *>(m_tabs->widget(i))) t->setSidebar(DocumentTab::Sidebar(mode));
    updateUi();
}

void MainWindow::updateUi()
{
    DocumentTab *t = currentTab();
    const bool loaded = t && t->isLoaded();
    for (QAction *a : {m_export, m_print, m_actual, m_zoomIn, m_zoomOut, m_fitPage, m_fitWidth, m_prevPage,
                       m_nextPage, m_firstPage, m_lastPage, m_goTo, m_rotLeft, m_rotRight, m_inspector, m_find,
                       m_findNext, m_findPrev})
        a->setEnabled(loaded);
    m_close->setEnabled(t);
    m_openDefault->setEnabled(t);
    m_reveal->setEnabled(t);
    m_search->setEnabled(loaded && t->document()->hasText());
    m_copy->setEnabled(loaded && ((t->view() && t->view()->hasSelection()) ||
                                  t->document()->kind() == DocumentKind::Image));
    m_hideSidebar->setChecked(m_sidebarMode == 0);
    m_thumbs->setChecked(m_sidebarMode == 1);
    m_toc->setChecked(m_sidebarMode == 2);
    m_zoomBox->setEnabled(loaded);

    if (!t) {
        setWindowTitle(QApplication::applicationDisplayName());
        setWindowFilePath({});
        m_pageLabel->clear();
        m_searchStatus->clear();
        return;
    }
    setWindowFilePath(t->path());
    setWindowTitle(t->title());
    if (!loaded) {
        m_pageLabel->setText(tr("Loading…"));
        return;
    }
    const int pages = t->document()->pageCount();
    QString page = pages > 1 ? tr("Page %1 of %2").arg(t->view()->currentPage() + 1).arg(pages)
                             : documentKindName(t->document()->kind());
    if (t->sniff().extensionMismatch) page += tr("  •  detected as %1").arg(documentKindName(t->document()->kind()));
    m_pageLabel->setText(page);
    if (!m_zoomBox->lineEdit()->hasFocus())
        m_zoomBox->setEditText(QStringLiteral("%1%").arg(qRound(t->view()->zoom() * 100)));
    if (t->searchText().isEmpty()) m_searchStatus->clear();
    else if (t->hitCount() == 0) m_searchStatus->setText(tr("Not found"));
    else m_searchStatus->setText(tr("%1 of %2").arg(t->currentHit() + 1).arg(t->hitCount()));
}

void MainWindow::openFile(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!QFileInfo(abs).isFile()) return;
    // Already open in any window: raise it.
    for (MainWindow *w : std::as_const(s_windows)) {
        for (int i = 0; i < w->m_tabs->count(); ++i) {
            if (auto *t = qobject_cast<DocumentTab *>(w->m_tabs->widget(i)); t && t->path() == abs) {
                w->m_tabs->setCurrentIndex(i);
                w->raise();
                w->activateWindow();
                return;
            }
        }
    }
    // "Give each file its own window": open in a fresh window unless this one is
    // still empty. A new (empty) window then takes the file through this path.
    if (Preferences::instance()->windowMode() == Preferences::WindowMode::Separate && m_tabs->count() > 0) {
        auto *w = new MainWindow;
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
        w->openFile(abs);
        return;
    }
    auto *tab = new DocumentTab(abs, m_tabs);
    tab->setSidebar(DocumentTab::Sidebar(m_sidebarMode));
    const int index = m_tabs->addTab(tab, QFileInfo(abs).fileName());
    // Tooltips auto-detect rich text: escape and force it.
    m_tabs->setTabToolTip(index, QStringLiteral("<p>%1</p>").arg(abs.toHtmlEscaped()));
    m_tabs->setCurrentIndex(index);
    connect(tab, &DocumentTab::stateChanged, this, [this, tab] {
        if (tab == currentTab()) updateUi();
    });
    connect(tab, &DocumentTab::loaded, this, [this, tab] {
        const int i = m_tabs->indexOf(tab);
        if (i >= 0 && tab->sniff().extensionMismatch)
            m_tabs->setTabToolTip(i, QStringLiteral("<p>%1<br>%2</p>")
                                         .arg(tab->path().toHtmlEscaped(),
                                              tr("Detected as %1").arg(documentKindName(tab->document()->kind()))));
        if (tab == currentTab()) updateUi();
    });
    connect(tab, &DocumentTab::exportRequested, this, [this] { exportPdf(); });

    QStringList recent = recentFiles();
    recent.removeAll(abs);
    recent.prepend(abs);
    QSettings().setValue(QStringLiteral("recentFiles"), recent.mid(0, 12));
    updateRecentMenu();
    updateUi();
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();
    for (const QString &f : recentFiles()) {
        if (!QFileInfo::exists(f)) continue;
        m_recentMenu->addAction(QFileInfo(f).fileName(), this, [this, f] { openFile(f); })->setToolTip(QStringLiteral("<p>%1</p>").arg(f.toHtmlEscaped()));
    }
    if (!m_recentMenu->isEmpty()) {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction(tr("Clear Menu"), this, [this] {
            QSettings().remove(QStringLiteral("recentFiles"));
            updateRecentMenu();
        });
    }
    m_recentMenu->setEnabled(!m_recentMenu->isEmpty());
}

void MainWindow::openDialog()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open"), s.value(QStringLiteral("lastDir"), QDir::homePath()).toString(), supportedFileFilter());
    if (files.isEmpty()) return;
    s.setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
    for (const QString &f : files) openFile(f);
}

void MainWindow::closeTab(int index)
{
    if (index < 0) return;
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    w->deleteLater();
    updateUi();
}

void MainWindow::exportPdf()
{
    DocumentTab *t = currentTab();
    if (!t || !t->isLoaded()) return;
    const QFileInfo fi(t->path());
    QString suggested = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pdf");
    if (QFileInfo(suggested).canonicalFilePath() == fi.canonicalFilePath())
        suggested = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + tr(" copy") + QStringLiteral(".pdf");
    QString path = QFileDialog::getSaveFileName(this, tr("Export as PDF"), suggested, tr("PDF document (*.pdf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) path += QStringLiteral(".pdf");

    DocumentPtr doc = t->document();
    const int rotation = t->view()->rotation();
    auto *progress = new QProgressDialog(tr("Exporting “%1”…").arg(t->title()), QString(), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(400);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, progress, path] {
        const QString error = watcher->result();
        progress->deleteLater();
        watcher->deleteLater();
        if (!error.isEmpty()) QMessageBox::warning(this, tr("Export Failed"), error);
        else statusBar()->showMessage(tr("Exported to %1").arg(QDir::toNativeSeparators(path)), 5000);
    });
    watcher->setFuture(QtConcurrent::run([doc, path, rotation]() -> QString {
        QString err;
        if (!Exporter::exportPdf(*doc, path, rotation, &err))
            return err.isEmpty() ? QStringLiteral("Could not write the PDF.") : err;
        return {};
    }));
}

void MainWindow::print()
{
    DocumentTab *t = currentTab();
    if (!t || !t->isLoaded()) return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(t->title());
    printer.setFromTo(1, t->document()->pageCount());
    QPrintDialog dialog(&printer, this);
    dialog.setMinMax(1, t->document()->pageCount());
    dialog.setOption(QAbstractPrintDialog::PrintPageRange);
    if (dialog.exec() != QDialog::Accepted) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Exporter::print(*t->document(), &printer, t->view()->rotation());
    QApplication::restoreOverrideCursor();
}

void MainWindow::copy()
{
    DocumentTab *t = currentTab();
    if (!t || !t->isLoaded()) return;
    if (t->view()->hasSelection()) {
        QApplication::clipboard()->setText(t->view()->selectedText());
        return;
    }
    if (auto *img = dynamic_cast<ImageDocument *>(t->document().get()))
        QApplication::clipboard()->setImage(img->frame(t->view()->currentPage()));
}

void MainWindow::goToPageDialog()
{
    DocumentTab *t = currentTab();
    if (!t || !t->isLoaded()) return;
    bool ok = false;
    const int n = t->document()->pageCount();
    const int page = QInputDialog::getInt(this, tr("Go to Page"), tr("Page (1–%1):").arg(n),
                                          t->view()->currentPage() + 1, 1, n, 1, &ok);
    if (ok) t->view()->goToPage(page - 1);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    for (const QUrl &url : e->mimeData()->urls())
        if (url.isLocalFile()) openFile(url.toLocalFile());
}

void MainWindow::showPreferences()
{
    if (!m_preferences) m_preferences = new PreferencesDialog(this);
    m_preferences->show();
    m_preferences->raise();
    m_preferences->activateWindow();
}

void MainWindow::paintEvent(QPaintEvent *e)
{
    // The top of the chrome gradient (behind the transparent title bar on
    // macOS); the toolbar paints the rest.
    if (!m_toolBar || !m_toolBar->isVisible()) return QMainWindow::paintEvent(e);
    const int top = m_toolBar->mapTo(this, QPoint(0, 0)).y();
    if (top <= 0) return;
    QPainter p(this);
    AppStyle::paintTitleGradient(&p, QRect(0, 0, width(), top), 0, top + m_toolBar->height(), isActiveWindow());
}

bool MainWindow::handleChromeMouse(QEvent *e)
{
    // The transparent title bar and the toolbar are our widgets, so the
    // system no longer drags the window by them: start the move ourselves
    // (native, with snapping), and zoom on double-click.
    if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonDblClick) return false;
    auto *me = static_cast<QMouseEvent *>(e);
    if (me->button() != Qt::LeftButton) return false;
    if (e->type() == QEvent::MouseButtonDblClick) {
        isMaximized() ? showNormal() : showMaximized();
        return true;
    }
    QWindow *w = windowHandle();
    if (w && w->startSystemMove()) return true;
    // The first click after launch reaches us when AppKit no longer reports
    // it as the current mouse-down, so the native move refuses it. Retry on
    // the first drag event, else move the window by hand until release.
    m_chromeDrag = true;
    m_chromeDragRetry = true;
    m_chromeDragOffset = me->globalPosition().toPoint() - pos();
    grabMouse();
    return true;
}

void MainWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_chromeDrag) return QMainWindow::mouseMoveEvent(e);
    if (m_chromeDragRetry) {
        m_chromeDragRetry = false;
        if (QWindow *w = windowHandle(); w && w->startSystemMove()) {
            m_chromeDrag = false;
            releaseMouse();
            return;
        }
    }
    move(e->globalPosition().toPoint() - m_chromeDragOffset);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_chromeDrag) return QMainWindow::mouseReleaseEvent(e);
    m_chromeDrag = false;
    releaseMouse();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_toolBar && handleChromeMouse(e)) return true;
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::mousePressEvent(QMouseEvent *e)
{
    // Presses reaching the window itself: the title strip, or anything a
    // child ignored. Only the title strip and the status bar move it.
    const bool chrome = (m_toolBar && e->position().y() < m_toolBar->geometry().top()) ||
                        statusBar()->geometry().contains(e->position().toPoint());
    if (!chrome || !handleChromeMouse(e)) QMainWindow::mousePressEvent(e);
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *e)
{
    const bool chrome = (m_toolBar && e->position().y() < m_toolBar->geometry().top()) ||
                        statusBar()->geometry().contains(e->position().toPoint());
    if (!chrome || !handleChromeMouse(e)) QMainWindow::mouseDoubleClickEvent(e);
}

void MainWindow::changeEvent(QEvent *e)
{
    // Chrome is lighter in inactive windows.
    if (e->type() == QEvent::ActivationChange) {
        update();
        if (m_toolBar) {
            m_toolBar->update();
            for (QWidget *w : m_toolBar->findChildren<QWidget *>()) w->update();
        }
        statusBar()->update();
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    QSettings().setValue(QStringLiteral("geometry"), saveGeometry());
    QMainWindow::closeEvent(e);
}
