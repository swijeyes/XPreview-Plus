// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "pdf/PdfDocument.h"

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QWidget;

// Sets a PDF's password protection: a password to open it, and/or
// restrictions on printing, copying and changes that an owner password lifts.
// The result is applied when the document is next saved.
class ProtectionDialog : public QDialog {
    Q_OBJECT
public:
    ProtectionDialog(const PdfDocument &doc, QWidget *parent = nullptr);
    PdfDocument::Protection protection() const { return m_result; }

private:
    void validate();
    void accept() override;

    QCheckBox *m_requireOpen;
    QLineEdit *m_open, *m_openVerify;
    QCheckBox *m_restrict;
    QCheckBox *m_print, *m_copy, *m_changes;
    QLineEdit *m_owner, *m_ownerVerify;
    QWidget *m_openFields, *m_restrictFields;
    QLabel *m_problem;
    QDialogButtonBox *m_buttons;
    PdfDocument::Protection m_result;
};
