// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QString>

class QWidget;

namespace SaveDialog {

// Asks where to save a PDF, with a "Protect with password" check box in the
// dialog. `protect` holds the box's state going in (shown) and coming out.
// `canProtect` false shows the box but disabled, with `disabledReason` as
// its tooltip. Returns the chosen path (ending in .pdf), or empty if
// cancelled. The macOS save panel is used there, Qt's dialog elsewhere.
QString getSavePdfFileName(QWidget *parent, const QString &title, const QString &suggested, bool *protect,
                           bool canProtect, const QString &disabledReason);

}  // namespace SaveDialog
