// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QString>

class FidelityReport;

namespace FontSubstitution {

// Registers bundled fonts (fonts/ next to the executable or in the macOS
// bundle's Resources). Call once at startup, from the GUI thread.
void loadBundledFonts();

// Maps a requested family to one that is available, recording any
// substitution in the report. Thread-safe.
QString resolve(const QString &family, FidelityReport *report);

// Maps a code point from a Symbol/Wingdings-encoded font to Unicode.
QChar mapSymbolChar(const QString &family, QChar c);
bool isSymbolFont(const QString &family);

}  // namespace FontSubstitution
