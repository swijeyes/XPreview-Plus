// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "DocumentLoader.h"

#include "flow/FlowDocument.h"
#include "image/ImageDocument.h"
#include "odf/OdtImporter.h"
#include "pdf/PdfDocument.h"
#include "rtf/RtfImporter.h"
#include "core/Limits.h"
#include "core/SafeImage.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <exception>

namespace {

// FlowDocument owns QObjects that live on the GUI thread. Worker jobs can
// hold the last reference, so the delete is always marshalled there.
void deleteOnGuiThread(FlowDocument *doc)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread()) {
        delete doc;
        return;
    }
    QMetaObject::invokeMethod(app, [doc] { delete doc; }, Qt::QueuedConnection);
}

DocumentPtr makeFlow(DocumentKind kind, flow::Document model, const QString &path)
{
    std::shared_ptr<FlowDocument> doc(new FlowDocument(kind, std::move(model), path), deleteOnGuiThread);
    // QTextDocument is a QObject: hand it to the GUI thread.
    if (QCoreApplication::instance()) doc->moveToThread(QCoreApplication::instance()->thread());
    return doc;
}

LoadResult load(const QString &path, const QByteArray &password)
{
    LoadResult r;
    r.sniff = sniffFile(path);
    QString error;
    switch (r.sniff.format) {
    case SniffedFormat::Pdf: {
        PdfDocument::LoadError e;
        r.document = PdfDocument::open(path, password, &e);
        if (e == PdfDocument::LoadError::Password) {
            r.needsPassword = true;
            r.error = QStringLiteral("This PDF is password protected.");
        } else if (!r.document) {
            r.error = QStringLiteral("The PDF could not be opened (it may be damaged).");
        }
        break;
    }
    case SniffedFormat::Image:
        r.document = ImageDocument::open(path, &error);
        break;
    case SniffedFormat::Svg:
        r.document = SvgDocument::open(path, &error);
        break;
    case SniffedFormat::Rtf: {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            error = f.errorString();
            break;
        }
        if (f.size() > Limits::kMaxTextFileBytes) {
            error = QStringLiteral("The file is too large to open.");
            break;
        }
        r.document = makeFlow(DocumentKind::Rtf, rtf::import(f.read(Limits::kMaxTextFileBytes)), path);
        break;
    }
    case SniffedFormat::OdfText:
    case SniffedFormat::FlatOdfText: {
        flow::Document model =
            odf::importOdt(path, r.sniff.format == SniffedFormat::FlatOdfText, &error);
        if (error.isEmpty() || !model.body.isEmpty())
            r.document = makeFlow(DocumentKind::Odt, std::move(model), path);
        break;
    }
    case SniffedFormat::OdfSpreadsheet:
        error = QStringLiteral("OpenDocument spreadsheets are not supported yet.");
        break;
    case SniffedFormat::OdfPresentation:
        error = QStringLiteral("OpenDocument presentations are not supported yet.");
        break;
    case SniffedFormat::Unknown:
        error = QStringLiteral("This file type is not supported.");
        break;
    }
    if (!r.document && r.error.isEmpty())
        r.error = error.isEmpty() ? QStringLiteral("The file could not be opened.") : error;
    if (r.document && r.document->pageCount() <= 0) {
        r.document.reset();
        r.error = QStringLiteral("The document has no pages.");
    }
    return r;
}

}  // namespace

LoadResult loadDocument(const QString &path, const QByteArray &password)
{
    SafeImage::initProcessLimits();
    QElapsedTimer timer;
    timer.start();
    LoadResult r;
    try {
        r = load(path, password);
    } catch (const std::exception &e) {
        // One bad file must never take down the other tabs.
        r.document.reset();
        r.error = QStringLiteral("Failed to open: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        r.document.reset();
        r.error = QStringLiteral("Failed to open the file.");
    }
    r.elapsedMs = timer.elapsed();
    return r;
}

QString supportedFileFilter()
{
    return QStringLiteral(
        "All supported files (*.pdf *.png *.jpg *.jpeg *.gif *.webp *.tif *.tiff *.bmp *.heic *.svg "
        "*.svgz *.rtf *.odt *.ott *.fodt);;"
        "PDF documents (*.pdf);;"
        "Images (*.png *.jpg *.jpeg *.gif *.webp *.tif *.tiff *.bmp *.heic *.svg *.svgz);;"
        "Text documents (*.rtf *.odt *.ott *.fodt);;"
        "All files (*)");
}
