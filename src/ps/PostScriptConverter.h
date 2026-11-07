// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QByteArray>
#include <QString>

// PostScript and EPS are programs, so they are never interpreted inside the
// viewer. Ghostscript converts them to PDF in a separate, short-lived
// process:
//   - -dSAFER (no file access beyond its input and output, no %pipe%),
//   - a clean environment (no GS_OPTIONS or other inherited settings),
//   - its own empty temporary directory holding a copy of the input,
//   - on macOS, a sandbox profile that denies network access, process
//     creation and writes outside that directory,
//   - a time limit, and size limits on the input and the PDF it produces.
// The resulting PDF is then opened like any other (from memory).
namespace PostScript {

// Ghostscript bundled with the app, else one installed on the system.
// Empty if there is none.
QString ghostscriptPath();

// Converts a PostScript or EPS file to PDF bytes. EPS is cropped to its
// bounding box. Safe to call from a worker thread.
QByteArray convertToPdf(const QString &path, bool eps, QString *error);

}  // namespace PostScript
