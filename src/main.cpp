// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "core/FontSubstitution.h"
#include "core/SafeImage.h"
#include "pdf/PdfDocument.h"
#include "ui/Appearance.h"
#include "ui/MainWindow.h"
#include "ui/Preferences.h"
#include "ui/RenderService.h"

#include <QApplication>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QSettings>
#include <QStyleHints>
#include <QIcon>
#include <QPointer>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>

#include <cstdio>

// Receives Finder "Open With" / double-click events on macOS.
class Application : public QApplication {
public:
    using QApplication::QApplication;
    QStringList pending;

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::FileOpen) {
            const QString file = static_cast<QFileOpenEvent *>(e)->file();
            if (MainWindow *w = MainWindow::front()) w->openFile(file);
            else pending << file;
            return true;
        }
        return QApplication::event(e);
    }
};

int main(int argc, char **argv)
{
    QApplication::setOrganizationName(QStringLiteral("XPreview"));
    QApplication::setOrganizationDomain(QStringLiteral("xpreview.org"));
    // The internal name keys settings (recent files, geometry); the display
    // name is what users see in titles, menus and the About box.
    QApplication::setApplicationName(QStringLiteral("XPreview"));
    QApplication::setApplicationDisplayName(QStringLiteral(XPREVIEW_DISPLAY_NAME));
    QApplication::setApplicationVersion(QStringLiteral(XPREVIEW_VERSION));
    Application app(argc, argv);
    // Portable mode: a file named "portable" next to the executable keeps
    // all settings in <app dir>/settings/ instead of the registry/home.
    const QString appDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(appDir + QStringLiteral("/portable"))) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, appDir + QStringLiteral("/settings"));
    }
    app.setQuitOnLastWindowClosed(true);
    // Appearance sets the colour scheme: Light for the Classic and Modern-Light
    // looks, Dark for Modern-Dark, and system-following for Modern-Automatic.
    Appearance::install();

    SafeImage::initProcessLimits();
    FontSubstitution::loadBundledFonts();
#ifndef Q_OS_MACOS
    // Linux/Windows window icon. Not on macOS: it would also replace the
    // Dock icon with this unmasked image instead of the bundle's .icns.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon-1024.png")));
#endif
    PdfDocument::initLibrary();

    auto *w = new MainWindow;
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    QStringList args = app.arguments().mid(1);
    // --page N: open the following file(s) at page N (used for documentation
    // screenshots). Relies on "return to last page" being on, as it is by
    // default.
    int startPage = 0;
    if (const int pg = args.indexOf(QStringLiteral("--page")); pg >= 0 && pg + 1 < args.size()) {
        startPage = args.takeAt(pg + 1).toInt();
        args.removeAt(pg);
    }
    // --screenshot OUT.png: grab the window once loaded, then quit (used for
    // UI smoke tests and docs).
    const int shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString out = args.takeAt(shot + 1);
        args.removeAt(shot);
        QTimer::singleShot(3000, w, [w, out] {
            w->grab().save(out);
            // Echo the status line so smoke tests can assert on it.
            if (auto *sb = w->findChild<QStatusBar *>())
                for (auto *label : sb->findChildren<QLabel *>())
                    if (!label->text().isEmpty()) fprintf(stdout, "status: %s\n", qPrintable(label->text()));
            QApplication::quit();
        });
    }
    for (const QString &arg : std::as_const(args))
        if (!arg.startsWith(QLatin1Char('-'))) {
            if (startPage > 0) Preferences::setLastPage(QFileInfo(arg).absoluteFilePath(), startPage - 1);
            w->openFile(arg);
        }
    for (const QString &f : std::as_const(app.pending)) w->openFile(f);
    app.pending.clear();
    const int rc = app.exec();
    // Finish background rendering while Qt is still fully alive.
    RenderService::shutdownAll();
    return rc;
}
