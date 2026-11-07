// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PdfSecurity.h"

#ifdef XPREVIEW_HAVE_QPDF
#include <qpdf/Buffer.hh>
#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <exception>
#include <memory>
#endif

namespace PdfSecurity {

#ifdef XPREVIEW_HAVE_QPDF

namespace {

QByteArray written(QPDFWriter &w)
{
    w.write();
    const std::shared_ptr<Buffer> buf = w.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(buf->getBuffer()), qsizetype(buf->getSize()));
}

void load(QPDF &pdf, const char *name, const QByteArray &data, const QByteArray &password = {})
{
    pdf.setSuppressWarnings(true);
    pdf.processMemoryFile(name, data.constData(), size_t(data.size()),
                          password.isEmpty() ? nullptr : password.constData());
}

}  // namespace

bool available() { return true; }

QByteArray encrypt(const QByteArray &plain, const QByteArray &userPassword, const QByteArray &ownerPassword,
                   const Permissions &allowed, QString *error)
{
    try {
        QPDF pdf;
        load(pdf, "document.pdf", plain);
        QPDFWriter w(pdf);
        w.setOutputMemory();
        w.setR6EncryptionParameters(userPassword.constData(), ownerPassword.constData(),
                                    /*accessibility*/ true, allowed.copy, allowed.changes, allowed.changes,
                                    allowed.changes, allowed.changes,
                                    allowed.print ? qpdf_r3p_full : qpdf_r3p_none, /*encrypt metadata*/ true);
        return written(w);
    } catch (const std::exception &e) {
        if (error) *error = QStringLiteral("The PDF could not be encrypted: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        if (error) *error = QStringLiteral("The PDF could not be encrypted.");
    }
    return {};
}

QByteArray encryptLike(const QByteArray &plain, const QByteArray &original, const QByteArray &password,
                       QString *error)
{
    try {
        QPDF source;
        load(source, "original.pdf", original, password);
        QPDF pdf;
        load(pdf, "document.pdf", plain);
        QPDFWriter w(pdf);
        w.setOutputMemory();
        w.copyEncryptionParameters(source);
        return written(w);
    } catch (const std::exception &e) {
        if (error) *error = QStringLiteral("The PDF could not be encrypted: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        if (error) *error = QStringLiteral("The PDF could not be encrypted.");
    }
    return {};
}

#else

bool available() { return false; }

QByteArray encrypt(const QByteArray &, const QByteArray &, const QByteArray &, const Permissions &, QString *error)
{
    if (error) *error = QStringLiteral("This build of XPreview+ cannot write encrypted PDFs.");
    return {};
}

QByteArray encryptLike(const QByteArray &, const QByteArray &, const QByteArray &, QString *error)
{
    if (error) *error = QStringLiteral("This build of XPreview+ cannot write encrypted PDFs.");
    return {};
}

#endif

}  // namespace PdfSecurity
