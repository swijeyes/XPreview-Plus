// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "SaveDialog.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QGridLayout>

namespace SaveDialog {

#ifndef Q_OS_MACOS
QString getSavePdfFileName(QWidget *parent, const QString &title, const QString &suggested, bool *protect,
                           bool canProtect, const QString &disabledReason)
{
    // The native dialogs of Windows and Linux desktops take no extra
    // controls through Qt, so this is Qt's own dialog.
    QFileDialog dialog(parent, title, suggested, QObject::tr("PDF document (*.pdf)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setDefaultSuffix(QStringLiteral("pdf"));
    dialog.selectFile(suggested);
    auto *box = new QCheckBox(QObject::tr("Protect with password"), &dialog);
    box->setObjectName(QStringLiteral("protectWithPassword"));
    box->setChecked(*protect);
    box->setEnabled(canProtect);
    if (!canProtect) box->setToolTip(disabledReason);
    if (auto *grid = qobject_cast<QGridLayout *>(dialog.layout()))
        grid->addWidget(box, grid->rowCount(), 0, 1, grid->columnCount());
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    *protect = box->isChecked();
    QString path = dialog.selectedFiles().first();
    if (!path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) path += QStringLiteral(".pdf");
    return path;
}
#endif

}  // namespace SaveDialog
