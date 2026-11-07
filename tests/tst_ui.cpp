// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "flow/FlowDocument.h"
#include "pdf/PdfDocument.h"
#include "ps/PostScriptConverter.h"
#include "ui/DocumentTab.h"
#include "ui/AppStyle.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/ProtectionDialog.h"
#include "ui/RenderService.h"
#include "ui/ThumbnailSidebar.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QPdfWriter>
#include <QScrollBar>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QtTest>

using namespace flow;

// Layout regressions. Run with classic scrollbars (Fusion): on macOS with
// "Show scroll bars: Always" a scrollbar appearing shrinks the viewport,
// which used to feed back into the thumbnail width forever.
class TestUi : public QObject {
    Q_OBJECT

    static DocumentPtr makeDoc(int paragraphs)
    {
        flow::Document model;
        model.page.size = QSizeF(300, 420);
        for (int i = 0; i < paragraphs; ++i) {
            Paragraph p;
            Run r;
            r.text = QStringLiteral("Paragraph %1 with some text to wrap across the line.").arg(i);
            p.runs << r;
            model.body << Block::paragraph(p);
        }
        return std::make_shared<FlowDocument>(DocumentKind::Rtf, model, QString());
    }

    static void settle()
    {
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
    }

private slots:
    void initTestCase()
    {
        // The app's custom scroll bars over Fusion: classic (space-taking)
        // scrollbars on every platform. XPREVIEW_NATIVE_STYLE=1 keeps the
        // platform style instead (on macOS combine with the
        // "-AppleShowScrollBars Always" argument to get classic scrollbars).
        if (qEnvironmentVariableIsEmpty("XPREVIEW_NATIVE_STYLE"))
            QApplication::setStyle(new AppStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    }

    // Same shutdown order as main(): render jobs must not outlive Qt.
    void cleanupTestCase() { RenderService::shutdownAll(); }

    // The window size is saved while resizing (not only on close), and the
    // next window opens at that size.
    void windowSizeRemembered()
    {
        QSettings().remove(QStringLiteral("geometry"));
        {
            MainWindow w;
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            w.resize(640, 480);
            QTRY_VERIFY(QSettings().contains(QStringLiteral("geometry")));
            MainWindow next;
            QCOMPARE(next.size(), QSize(640, 480));
        }
        QSettings().remove(QStringLiteral("geometry"));
    }

    void thumbnailSidebarResizeSweep_data()
    {
        QTest::addColumn<int>("paragraphs");
        QTest::newRow("1 page") << 10;
        QTest::newRow("2 pages") << 30;
        QTest::newRow("3 pages") << 45;
        QTest::newRow("many pages") << 200;
    }

    void thumbnailSidebarResizeSweep()
    {
        // The feedback loop needs content height ~ viewport height, so sweep
        // both dimensions for short documents: every width, many heights.
        QFETCH(int, paragraphs);
        DocumentPtr doc = makeDoc(paragraphs);
        RenderService renderer(doc);
        ThumbnailSidebar sidebar(doc, &renderer);
        sidebar.setResizeDebounce(0);  // exercise the deferred relayout on every step
        sidebar.show();
        const int page = doc->pageCount() - 1;
        sidebar.setCurrentPage(page);
        for (int h = 100; h <= 1000; h += 12) {
            for (int w = 80; w <= 460; ++w) {
                sidebar.resize(w, h);
                settle();
                // Must converge: no pending relayout, no ping-pong between widths.
                const int width = sidebar.thumbnailWidth();
                settle();
                if (sidebar.layoutPending() || sidebar.thumbnailWidth() != width)
                    QFAIL(qPrintable(QStringLiteral("thumbnail width did not settle at %1x%2").arg(w).arg(h)));
            }
        }
        QCOMPARE(sidebar.currentIndex().row(), page);
    }

    void documentViewResizeSweep_data()
    {
        QTest::addColumn<int>("paragraphs");
        QTest::addColumn<int>("step");
        QTest::newRow("1 page") << 10 << 2;
        QTest::newRow("2 pages") << 30 << 2;
        QTest::newRow("many pages") << 200 << 7;  // slower layout: coarser sweep
    }

    void documentViewResizeSweep()
    {
        // Fit-to-width depends on whether the vertical scrollbar is shown,
        // and the zoom decides whether it is: sweep near that boundary.
        QFETCH(int, paragraphs);
        QFETCH(int, step);
        DocumentPtr doc = makeDoc(paragraphs);
        RenderService renderer(doc);
        DocumentView view(doc, &renderer);
        view.show();
        for (auto mode : {DocumentView::FitMode::Width, DocumentView::FitMode::Page}) {
            view.setFitMode(mode);
            for (int h = 150; h <= 1200; h += 12 * step) {
                for (int w = 120; w <= 900; w += step) {
                    view.resize(w, h);
                    settle();
                    const double z = view.zoom();
                    settle();
                    if (view.zoom() != z)
                        QFAIL(qPrintable(QStringLiteral("zoom did not settle at %1x%2").arg(w).arg(h)));
                }
            }
        }
        QVERIFY(view.zoom() > 0);
    }

    void displayModes()
    {
        DocumentPtr doc = makeDoc(200);
        QVERIFY(doc->pageCount() >= 10);
        RenderService renderer(doc);
        DocumentView view(doc, &renderer);
        // Before the view is shown: remembered, applied once it has a size.
        view.goToPage(7);
        QCOMPARE(view.currentPage(), 7);
        view.resize(800, 600);
        view.show();
        QTRY_VERIFY(view.verticalScrollBar()->value() > 0);
        QCOMPARE(view.currentPage(), 7);

        view.setDisplayMode(DocumentView::DisplayMode::SinglePage);
        QCOMPARE(view.currentPage(), 7);
        view.stepPage(1);
        QCOMPARE(view.currentPage(), 8);
        view.stepPage(-1);
        QCOMPARE(view.currentPage(), 7);

        view.setDisplayMode(DocumentView::DisplayMode::TwoPages);
        view.stepPage(1);  // spread 6-7 -> 8-9
        QCOMPARE(view.currentPage(), 8);
        view.stepPage(-1);
        QCOMPARE(view.currentPage(), 6);
        view.goToPage(0);
        view.stepPage(-1);  // already first
        QCOMPARE(view.currentPage(), 0);

        // Fit width must settle with two pages side by side, too.
        view.setFitMode(DocumentView::FitMode::Width);
        for (int w = 200; w <= 900; w += 9) {
            view.resize(w, 500);
            settle();
            const double z = view.zoom();
            settle();
            if (view.zoom() != z) QFAIL(qPrintable(QStringLiteral("two-page zoom did not settle at width %1").arg(w)));
        }
        view.setDisplayMode(DocumentView::DisplayMode::Continuous);
        view.goToPage(4);
        QCOMPARE(view.currentPage(), 4);
    }

    void physicalScale()
    {
        DocumentPtr doc = makeDoc(10);
        RenderService renderer(doc);
        DocumentView view(doc, &renderer);
        view.show();
        view.setPhysicalScale(true);
        QVERIFY(view.unitScale() >= 0.5 && view.unitScale() <= 3.0);
        view.actualSize();
        QCOMPARE(view.zoom(), 1.0);  // 100% is still "100%", whatever it means
        view.setPhysicalScale(false);
        QCOMPARE(view.unitScale(), 1.0);
    }

    void documentTabResizeSweep()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("t.rtf"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QByteArray rtf = "{\\rtf1 ";
        for (int i = 0; i < 400; ++i) rtf += "Paragraph " + QByteArray::number(i) + " of filler text.\\par ";
        f.write(rtf + "}");
        f.close();
        DocumentTab tab(path);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isLoaded(), 10000);
        tab.show();
        for (auto mode : {DocumentTab::Sidebar::Thumbnails, DocumentTab::Sidebar::Outline}) {
            tab.setSidebar(mode);
            for (int w = 300; w <= 1400; w += 7) {
                tab.resize(w, 200 + w / 2);
                settle();
            }
        }
    }

    // PostScript is converted in memory; its pages, not the .eps bytes,
    // must be inserted.
    void insertEpsIntoPdf()
    {
        if (PostScript::ghostscriptPath().isEmpty()) QSKIP("Ghostscript not installed");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("form.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(XPREVIEW_TEST_CORPUS "/form.pdf"), path));
        DocumentTab tab(path);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isLoaded(), 10000);
        const int pages = tab.pdf()->pageCount();
        tab.insertFiles({QStringLiteral(XPREVIEW_TEST_CORPUS "/sample.eps")}, pages);
        QCOMPARE(tab.pdf()->pageCount(), pages + 1);
    }

    void pdfFormFillingInTab()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("form.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(XPREVIEW_TEST_CORPUS "/form.pdf"), path));
        DocumentTab tab(path);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isLoaded(), 10000);
        tab.resize(900, 900);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));
        settle();
        QVERIFY(tab.isEditable());
        DocumentView *view = tab.view();
        QSignalSpy modified(&tab, &DocumentTab::modifiedChanged);
        // Click into the "name" field, (160,650)-(460,674) in PDF space.
        const QPoint name = view->mapFromPage(0, QPointF(300, 792 - 662)).toPoint();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, name);
        QVERIFY(view->formHasFocus());
        QTest::keyClicks(view, QStringLiteral("Ada Lovelacx"));
        QTest::keyClick(view, Qt::Key_Backspace);
        QTest::keyClicks(view, QStringLiteral("e"));
        // Clicking off the field ends editing.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->mapFromPage(0, QPointF(500, 60)).toPoint());
        QVERIFY(!view->formHasFocus());
        QCOMPARE(tab.pdf()->formValues().value(QStringLiteral("name")), QStringLiteral("Ada Lovelace"));
        QTRY_VERIFY(tab.isModified());
        QTRY_VERIFY(!modified.isEmpty());
        QVERIFY(tab.save());
        QVERIFY(!tab.isModified());
    }

    void pdfPageEditingInTab()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pages.pdf"));
        {
            QPdfWriter w(path);
            w.setResolution(72);
            w.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point, QString(), QPageSize::ExactMatch));
            w.setPageMargins({});
            QPainter p(&w);
            for (int i = 0; i < 3; ++i) {
                if (i) w.newPage();
                p.drawText(QPointF(40, 60), QStringLiteral("Page %1").arg(i + 1));
            }
        }
        const QString png = dir.filePath(QStringLiteral("pic.png"));
        QImage img(200, 100, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(png));

        DocumentTab tab(path);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isLoaded(), 10000);
        tab.resize(900, 700);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));
        settle();
        auto *thumbs = tab.findChild<ThumbnailSidebar *>();
        QVERIFY(thumbs && thumbs->isEditable());
        auto pageText = [&](int i) { return tab.document()->pageText(i).trimmed(); };

        tab.movePages({2}, 0);
        QCOMPARE(pageText(0), QStringLiteral("Page 3"));
        QCOMPARE(thumbs->selectedPages(), QVector<int>({0}));
        QVERIFY(tab.isModified());

        // Files dropped on the sidebar (filesDropped) are inserted where they
        // land: an image becomes a page of its own. (Qt delivers drop events
        // only during a real drag, so the drop itself is not simulated.)
        QVERIFY(thumbs->acceptDrops());
        tab.insertFiles({png}, 3);
        QCOMPARE(tab.document()->pageCount(), 4);
        QCOMPARE(thumbs->model()->rowCount(), 4);
        QCOMPARE(tab.document()->pageSize(3).width(), 2 * tab.document()->pageSize(3).height());  // the picture's shape

        thumbs->selectionModel()->select(thumbs->model()->index(3, 0), QItemSelectionModel::ClearAndSelect);
        tab.deletePages();
        QCOMPARE(tab.document()->pageCount(), 3);
        tab.undo();
        QCOMPARE(tab.document()->pageCount(), 4);
        QCOMPARE(thumbs->model()->rowCount(), 4);
        QVERIFY(tab.save());
        QVERIFY(!tab.isModified());
    }

    void protectionDialogValidates()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(XPREVIEW_TEST_CORPUS "/form.pdf"), path));
        PdfDocument::LoadError e;
        auto doc = PdfDocument::open(path, {}, &e);
        QVERIFY(doc);
        ProtectionDialog dialog(*doc);
        auto edits = dialog.findChildren<QLineEdit *>();
        QCOMPARE(edits.size(), 4);  // open, verify, owner, verify
        QPushButton *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QVERIFY(!ok->isEnabled());  // no password yet
        edits[0]->setText(QStringLiteral("abc"));
        QVERIFY(!ok->isEnabled());  // not verified
        edits[1]->setText(QStringLiteral("abc"));
        QVERIFY(ok->isEnabled());
        // Restricting needs a different owner password.
        auto boxes = dialog.findChildren<QCheckBox *>();
        QCheckBox *restrict = nullptr, *print = nullptr;
        for (QCheckBox *b : boxes) {
            if (b->text().startsWith(QStringLiteral("Restrict"))) restrict = b;
            if (b->text() == QStringLiteral("Allow printing")) print = b;
        }
        QVERIFY(restrict && print);
        restrict->setChecked(true);
        print->setChecked(false);
        QVERIFY(!ok->isEnabled());
        edits[2]->setText(QStringLiteral("abc"));
        edits[3]->setText(QStringLiteral("abc"));
        QVERIFY(!ok->isEnabled());  // same as the open password
        edits[2]->setText(QStringLiteral("owner"));
        edits[3]->setText(QStringLiteral("owner"));
        QVERIFY(ok->isEnabled());
        ok->click();
        const PdfDocument::Protection p = dialog.protection();
        QCOMPARE(p.mode, PdfDocument::Protection::Mode::Custom);
        QCOMPARE(p.openPassword, QByteArray("abc"));
        QCOMPARE(p.ownerPassword, QByteArray("owner"));
        QVERIFY(!p.allowPrint && p.allowCopy && p.allowChanges);
    }
};

QTEST_MAIN(TestUi)
#include "tst_ui.moc"
