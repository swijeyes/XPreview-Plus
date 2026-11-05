// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QDialog>

class QWidget;

// Application preferences, as a tabbed window (General, PDF, Images).
// Changes apply immediately, as on macOS.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

private:
    QWidget *buildGeneralTab();
    QWidget *buildPdfTab();
    QWidget *buildImagesTab();
};
