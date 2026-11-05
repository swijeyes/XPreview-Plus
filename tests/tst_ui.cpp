// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "flow/FlowDocument.h"
#include "ui/DocumentTab.h"
#include "ui/AppStyle.h"
#include "ui/DocumentView.h"
#include "ui/RenderService.h"
#include "ui/ThumbnailSidebar.h"

#include <QApplication>
#include <QScrollBar>
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
};

QTEST_MAIN(TestUi)
#include "tst_ui.moc"
