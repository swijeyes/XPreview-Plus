// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PdfSecurity.h"

#ifdef XPREVIEW_HAVE_QPDF
#include <qpdf/Buffer.hh>
#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
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

Decrypted decrypt(const QByteArray &encrypted, const QByteArray &password)
{
    Decrypted out;
    try {
        QPDF pdf;
        load(pdf, "document.pdf", encrypted, password);
        int R = 0, P = 0, V = 0;
        QPDF::encryption_method_e stream, string, file;
        if (!pdf.isEncrypted(R, P, V, stream, string, file)) {
            out.result = Decrypted::Result::Unsupported;  // not encrypted: nothing for qpdf to fix
            return out;
        }
        out.revision = R;
        out.permissions = pdf.ownerPasswordMatched() ? 0xffffffff : static_cast<unsigned long>(static_cast<unsigned>(P));
        QPDFWriter w(pdf);
        w.setOutputMemory();
        w.setPreserveEncryption(false);
        out.pdf = written(w);
        out.result = out.pdf.isEmpty() ? Decrypted::Result::Failed : Decrypted::Result::Ok;
    } catch (const QPDFExc &e) {
        out.result = e.getErrorCode() == qpdf_e_password ? Decrypted::Result::Password : Decrypted::Result::Failed;
        // qpdf reports unknown security handlers (certificates, DRM) as unsupported.
        if (out.result == Decrypted::Result::Failed && e.getErrorCode() == qpdf_e_unsupported)
            out.result = Decrypted::Result::Unsupported;
    } catch (...) {
        out.result = Decrypted::Result::Failed;
    }
    return out;
}

#else

bool available() { return false; }

Decrypted decrypt(const QByteArray &, const QByteArray &) { return {}; }

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
