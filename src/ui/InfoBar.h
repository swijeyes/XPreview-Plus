// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QFrame>

class QLabel;
class QHBoxLayout;

// Dismissible bar shown above a document, e.g. "Simplified view".
class InfoBar : public QFrame {
    Q_OBJECT
public:
    explicit InfoBar(QWidget *parent = nullptr);
    void setMessage(const QString &text);
    void addAction(const QString &label, std::function<void()> fn);

private:
    QLabel *m_label;
    QHBoxLayout *m_buttons;
};
