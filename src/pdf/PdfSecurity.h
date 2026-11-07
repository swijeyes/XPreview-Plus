// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QByteArray>
#include <QString>

// Writing encrypted PDFs. PDFium reads every standard security handler but
// cannot encrypt, so this goes through qpdf when the build has it. Inputs are
// always PDFs this program produced itself (PDFium's own output).
namespace PdfSecurity {

// False when built without qpdf: encrypted PDFs then open but cannot be saved.
bool available();

struct Permissions {
    bool print = true;
    bool copy = true;
    bool changes = true;  // page assembly, form filling, annotations, edits
};

// Encrypts `plain` with AES-256 (PDF 2.0 / revision 6). `userPassword` opens
// the file (empty: anyone can open it); `ownerPassword` lifts `allowed`.
QByteArray encrypt(const QByteArray &plain, const QByteArray &userPassword, const QByteArray &ownerPassword,
                   const Permissions &allowed, QString *error);

// Encrypts `plain` exactly like `original`, an encrypted PDF that `password`
// opens: same passwords, permissions and algorithm.
QByteArray encryptLike(const QByteArray &plain, const QByteArray &original, const QByteArray &password,
                       QString *error);

}  // namespace PdfSecurity
