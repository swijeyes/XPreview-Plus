// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PostScriptConverter.h"

#include "core/Limits.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace PostScript {

namespace {

QString appDir() { return QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString(); }

// Ghostscript shipped inside the app, and its resource folder.
QString bundledGhostscript()
{
    const QString dir = appDir();
    if (dir.isEmpty()) return {};
#if defined(Q_OS_MACOS)
    const QString gs = dir + QStringLiteral("/../Helpers/gs");
#elif defined(Q_OS_WIN)
    const QString gs = dir + QStringLiteral("/ghostscript/bin/gswin64c.exe");
#else
    const QString gs = dir + QStringLiteral("/../libexec/xpreview/gs");
#endif
    const QFileInfo fi(gs);
    return fi.isFile() && fi.isExecutable() ? fi.canonicalFilePath() : QString();
}

QString bundledResources()
{
#if defined(Q_OS_MACOS)
    return QDir(appDir() + QStringLiteral("/../Resources/ghostscript")).canonicalPath();
#elif defined(Q_OS_WIN)
    return QDir(appDir() + QStringLiteral("/ghostscript")).canonicalPath();
#else
    return QDir(appDir() + QStringLiteral("/../share/xpreview/ghostscript")).canonicalPath();
#endif
}

// A string literal in Apple's sandbox profile language.
QString sbplString(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

}  // namespace

QString ghostscriptPath()
{
    if (const QString gs = bundledGhostscript(); !gs.isEmpty()) return gs;
#ifdef Q_OS_WIN
    for (const char *name : {"gswin64c", "gswin32c"})
        if (const QString gs = QStandardPaths::findExecutable(QString::fromLatin1(name)); !gs.isEmpty()) return gs;
#else
    // Apps started from the Finder get a minimal PATH: also look where
    // package managers install it.
    if (const QString gs = QStandardPaths::findExecutable(QStringLiteral("gs"),
                                                          {QStringLiteral("/opt/homebrew/bin"),
                                                           QStringLiteral("/usr/local/bin"),
                                                           QStringLiteral("/opt/local/bin"),
                                                           QStringLiteral("/usr/bin")});
        !gs.isEmpty())
        return QFileInfo(gs).canonicalFilePath();
#endif
    return {};
}

QByteArray convertToPdf(const QString &path, bool eps, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QByteArray();
    };
    const QString gs = ghostscriptPath();
    if (gs.isEmpty())
        return fail(QStringLiteral("Opening PostScript and EPS files needs Ghostscript, which was not found."));
    const QFileInfo in(path);
    if (!in.isFile()) return fail(QStringLiteral("The file could not be read."));
    if (in.size() > Limits::kMaxPostScriptBytes) return fail(QStringLiteral("The file is too large to open."));

    // Private working folder (mode 0700) with a copy of the input under a
    // fixed name, so nothing about the original file name reaches
    // Ghostscript's command line.
    QTemporaryDir work(QDir::tempPath() + QStringLiteral("/xpreview-ps-XXXXXX"));
    if (!work.isValid()) return fail(QStringLiteral("Could not create a temporary folder."));
    const QString dir = QDir(work.path()).canonicalPath();
    // "%" in an output path is a page-number placeholder for Ghostscript.
    if (dir.isEmpty() || dir.contains(QLatin1Char('%'))) return fail(QStringLiteral("Could not create a temporary folder."));
    const QString input = dir + QStringLiteral("/input.ps");
    const QString output = dir + QStringLiteral("/output.pdf");
    if (!QFile::copy(path, input)) return fail(QStringLiteral("The file could not be read."));

    QStringList args = {
        QStringLiteral("-q"),
        QStringLiteral("-dSAFER"),
        QStringLiteral("-dBATCH"),
        QStringLiteral("-dNOPAUSE"),
        QStringLiteral("-dNOPROMPT"),
        QStringLiteral("-sDEVICE=pdfwrite"),
        QStringLiteral("-dCompatibilityLevel=1.7"),
        QStringLiteral("-dAutoRotatePages=/None"),
        QStringLiteral("--permit-file-read=") + dir + QLatin1Char('/'),
        QStringLiteral("--permit-file-write=") + dir + QLatin1Char('/'),
    };
    if (eps) args << QStringLiteral("-dEPSCrop");

    // Only what Ghostscript needs; nothing inherited (GS_OPTIONS could add
    // -dNOSAFER).
    QProcessEnvironment env;
#ifdef Q_OS_WIN
    const QProcessEnvironment sys = QProcessEnvironment::systemEnvironment();
    for (const char *key : {"SystemRoot", "windir"})
        if (sys.contains(QString::fromLatin1(key))) env.insert(QString::fromLatin1(key), sys.value(QString::fromLatin1(key)));
    env.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(dir));
    env.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(dir));
    const QChar sep = QLatin1Char(';');
#else
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("HOME"), dir);
    env.insert(QStringLiteral("TMPDIR"), dir);
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    const QChar sep = QLatin1Char(':');
#endif
    const bool bundled = gs == bundledGhostscript();
    const QString res = bundled ? bundledResources() : QString();
    if (!res.isEmpty()) {
        env.insert(QStringLiteral("GS_LIB"),
                   QStringList{res + QStringLiteral("/Resource/Init"), res + QStringLiteral("/lib"),
                               res + QStringLiteral("/Resource/Font"), res + QStringLiteral("/fonts")}
                       .join(sep));
        args << QStringLiteral("-sGenericResourceDir=") + res + QStringLiteral("/Resource/")
             << QStringLiteral("-sICCProfilesDir=") + res + QStringLiteral("/iccprofiles/");
    }
    args << QStringLiteral("-sOutputFile=") + output << QStringLiteral("-f") << input;

    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setWorkingDirectory(dir);
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.setStandardOutputFile(QProcess::nullDevice());
    proc.setStandardErrorFile(QProcess::nullDevice());  // unbounded output must not fill memory or disk
#ifdef Q_OS_UNIX
    proc.setChildProcessModifier([] {
        // Bounded CPU time and file size; no core dumps.
        const rlimit cpu{90, 95};
        const rlimit fsize{rlim_t(Limits::kMaxPostScriptPdfBytes), rlim_t(Limits::kMaxPostScriptPdfBytes)};
        const rlimit core{0, 0};
        setrlimit(RLIMIT_CPU, &cpu);
        setrlimit(RLIMIT_FSIZE, &fsize);
        setrlimit(RLIMIT_CORE, &core);
#ifndef Q_OS_MACOS
        const rlimit as{rlim_t(4) << 30, rlim_t(4) << 30};
        setrlimit(RLIMIT_AS, &as);
#endif
    });
#endif

    QString program = gs;
#ifdef Q_OS_MACOS
    const QString sandbox = QStringLiteral("/usr/bin/sandbox-exec");
    if (QFileInfo(sandbox).isExecutable()) {
        // Everything not explicitly denied stays as Ghostscript expects.
        // Reading is allowed outside the home folder (system fonts, the
        // app), and inside it only for the work folder and the app.
        const QString home = QDir(QDir::homePath()).canonicalPath();
        const QString app = QDir(appDir() + QStringLiteral("/..")).canonicalPath();
        const QString profile =
            QStringLiteral("(version 1)\n"
                           "(allow default)\n"
                           "(deny network*)\n"
                           "(deny process-fork)\n"
                           "(deny file-write* (require-not (require-any (subpath %1) (literal \"/dev/null\")"
                           " (literal \"/dev/dtracehelper\"))))\n"
                           "(deny file-read* (require-all (subpath %2) (require-not (require-any (subpath %1)"
                           " (subpath %3) (subpath %4)))))\n")
                .arg(sbplString(dir), sbplString(home), sbplString(app), sbplString(QFileInfo(gs).absolutePath()));
        args.prepend(gs);
        args.prepend(profile);
        args.prepend(QStringLiteral("-p"));
        program = sandbox;
    }
#endif

    proc.start(program, args);
    if (!proc.waitForStarted(10000)) return fail(QStringLiteral("Ghostscript could not be started."));
    QElapsedTimer timer;
    timer.start();
    // Poll so a runaway job can be stopped by output size as well as time.
    while (!proc.waitForFinished(250)) {
        const bool tooLong = timer.elapsed() > Limits::kPostScriptTimeoutMs;
        const bool tooBig = QFileInfo(output).size() > Limits::kMaxPostScriptPdfBytes;
        if (tooLong || tooBig || proc.state() == QProcess::NotRunning) {
            proc.kill();
            proc.waitForFinished(3000);
            if (tooLong) return fail(QStringLiteral("The PostScript file took too long to convert."));
            if (tooBig) return fail(QStringLiteral("The PostScript file is too large to convert."));
            break;
        }
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return fail(QStringLiteral("The PostScript file could not be converted (it may be damaged)."));
    QFile out(output);
    if (!out.open(QIODevice::ReadOnly) || out.size() > Limits::kMaxPostScriptPdfBytes)
        return fail(QStringLiteral("The PostScript file could not be converted."));
    QByteArray pdf = out.readAll();
    if (!pdf.startsWith("%PDF-")) return fail(QStringLiteral("The PostScript file could not be converted."));
    return pdf;
}

}  // namespace PostScript
