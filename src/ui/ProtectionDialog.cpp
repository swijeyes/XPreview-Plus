// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "ProtectionDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QLineEdit *passwordField(QWidget *parent)
{
    auto *e = new QLineEdit(parent);
    e->setEchoMode(QLineEdit::Password);
    e->setMinimumWidth(220);
    // Never offered to input methods' history or autocompletion.
    e->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhHiddenText);
    e->setMaxLength(127);  // AES-256 passwords are at most 127 bytes of UTF-8
    return e;
}

QLabel *note(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() - 1);
    l->setFont(f);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

}  // namespace

ProtectionDialog::ProtectionDialog(const PdfDocument &doc, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Password Protection"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    if (doc.protection().mode == PdfDocument::Protection::Mode::Original)
        layout->addWidget(note(tr("This PDF is protected (%1). Saving keeps that protection unless you choose "
                                  "new settings here.")
                                   .arg(doc.encryptionDescription()),
                               this));

    m_requireOpen = new QCheckBox(tr("Require a password to open this PDF"), this);
    layout->addWidget(m_requireOpen);
    m_openFields = new QWidget(this);
    auto *openForm = new QFormLayout(m_openFields);
    openForm->setContentsMargins(24, 0, 0, 0);
    m_open = passwordField(m_openFields);
    m_openVerify = passwordField(m_openFields);
    openForm->addRow(tr("Password:"), m_open);
    openForm->addRow(tr("Verify:"), m_openVerify);
    layout->addWidget(m_openFields);

    m_restrict = new QCheckBox(tr("Restrict printing, copying and changes"), this);
    layout->addWidget(m_restrict);
    m_restrictFields = new QWidget(this);
    auto *restrictLayout = new QVBoxLayout(m_restrictFields);
    restrictLayout->setContentsMargins(24, 0, 0, 0);
    m_print = new QCheckBox(tr("Allow printing"), m_restrictFields);
    m_copy = new QCheckBox(tr("Allow copying text and images"), m_restrictFields);
    m_changes = new QCheckBox(tr("Allow changes (pages, form filling, comments)"), m_restrictFields);
    for (QCheckBox *c : {m_print, m_copy, m_changes}) restrictLayout->addWidget(c);
    auto *ownerForm = new QFormLayout;
    m_owner = passwordField(m_restrictFields);
    m_ownerVerify = passwordField(m_restrictFields);
    ownerForm->addRow(tr("Owner password:"), m_owner);
    ownerForm->addRow(tr("Verify:"), m_ownerVerify);
    restrictLayout->addLayout(ownerForm);
    restrictLayout->addWidget(note(tr("The owner password lifts the restrictions. Other PDF apps honour them; "
                                      "they are not a substitute for an open password."),
                                   m_restrictFields));
    layout->addWidget(m_restrictFields);

    layout->addWidget(note(tr("Protection uses AES-256 encryption and is applied when you save. If you forget "
                              "the password, the PDF cannot be opened again."),
                           this));
    m_problem = new QLabel(this);
    m_problem->setTextFormat(Qt::PlainText);
    m_problem->setStyleSheet(QStringLiteral("color: #d33;"));
    layout->addWidget(m_problem);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (doc.protection().mode != PdfDocument::Protection::Mode::None) {
        QPushButton *remove = m_buttons->addButton(tr("Remove Protection"), QDialogButtonBox::DestructiveRole);
        connect(remove, &QPushButton::clicked, this, [this] {
            m_result = {};
            QDialog::accept();
        });
    }
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ProtectionDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    // Start from the current custom settings, if any.
    const PdfDocument::Protection &cur = doc.protection();
    const bool custom = cur.mode == PdfDocument::Protection::Mode::Custom;
    m_requireOpen->setChecked(custom ? !cur.openPassword.isEmpty() : true);
    m_open->setText(custom ? QString::fromUtf8(cur.openPassword) : QString());
    m_openVerify->setText(m_open->text());
    m_restrict->setChecked(custom && cur.restricts());
    m_print->setChecked(!custom || cur.allowPrint);
    m_copy->setChecked(!custom || cur.allowCopy);
    m_changes->setChecked(!custom || cur.allowChanges);
    m_owner->setText(custom ? QString::fromUtf8(cur.ownerPassword) : QString());
    m_ownerVerify->setText(m_owner->text());

    for (QCheckBox *c : {m_requireOpen, m_restrict, m_print, m_copy, m_changes})
        connect(c, &QCheckBox::toggled, this, &ProtectionDialog::validate);
    for (QLineEdit *e : {m_open, m_openVerify, m_owner, m_ownerVerify})
        connect(e, &QLineEdit::textChanged, this, &ProtectionDialog::validate);
    validate();
    m_open->setFocus();
}

void ProtectionDialog::validate()
{
    m_openFields->setEnabled(m_requireOpen->isChecked());
    m_restrictFields->setEnabled(m_restrict->isChecked());
    QString problem;
    const bool restricting = m_restrict->isChecked() &&
                             !(m_print->isChecked() && m_copy->isChecked() && m_changes->isChecked());
    if (!m_requireOpen->isChecked() && !restricting)
        problem = tr("Choose a password, restrictions, or both.");
    else if (m_requireOpen->isChecked() && m_open->text().isEmpty())
        problem = tr("Enter a password to open the PDF.");
    else if (m_requireOpen->isChecked() && m_open->text() != m_openVerify->text())
        problem = tr("The passwords do not match.");
    else if (restricting && m_owner->text().isEmpty())
        problem = tr("Enter an owner password to lift the restrictions.");
    else if (restricting && m_owner->text() != m_ownerVerify->text())
        problem = tr("The owner passwords do not match.");
    else if (restricting && m_requireOpen->isChecked() && m_owner->text() == m_open->text())
        problem = tr("The owner password must differ from the password to open.");
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}

void ProtectionDialog::accept()
{
    PdfDocument::Protection p;
    p.mode = PdfDocument::Protection::Mode::Custom;
    if (m_requireOpen->isChecked()) p.openPassword = m_open->text().toUtf8();
    if (m_restrict->isChecked()) {
        p.allowPrint = m_print->isChecked();
        p.allowCopy = m_copy->isChecked();
        p.allowChanges = m_changes->isChecked();
        if (p.restricts()) p.ownerPassword = m_owner->text().toUtf8();
    }
    m_result = p;
    QDialog::accept();
}
