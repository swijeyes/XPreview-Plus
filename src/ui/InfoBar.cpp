// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "InfoBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

InfoBar::InfoBar(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("InfoBar"));
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    setStyleSheet(dark ? QStringLiteral("#InfoBar { background: #4a3f1c; border-bottom: 1px solid #6b5a22; }"
                                        "#InfoBar QLabel { color: #f2e6c2; }")
                       : QStringLiteral("#InfoBar { background: #fff4cc; border-bottom: 1px solid #e6d28a; }"
                                        "#InfoBar QLabel { color: #4d3d00; }"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 6, 6);
    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    // Messages can include text derived from the document: never rich text.
    m_label->setTextFormat(Qt::PlainText);
    layout->addWidget(m_label, 1);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(6);
    layout->addLayout(m_buttons);
    auto *close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setToolTip(tr("Dismiss"));
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    layout->addWidget(close);
}

void InfoBar::setMessage(const QString &text) { m_label->setText(text); }

void InfoBar::addAction(const QString &label, std::function<void()> fn)
{
    auto *b = new QPushButton(label, this);
    b->setAutoDefault(false);
    connect(b, &QPushButton::clicked, this, [fn = std::move(fn)] { fn(); });
    m_buttons->addWidget(b);
}
