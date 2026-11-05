// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "Document.h"

class QPrinter;

namespace Exporter {

// Writes any document as PDF. PDFs are copied losslessly (rotation applied
// through PDFium); every other format is painted page by page as vector
// output through QPdfWriter. `rotation` is the view rotation in degrees.
bool exportPdf(Document &doc, const QString &path, int rotation, QString *error);

// Prints through the vector paintPage() path, fitting each page to the
// printer's paper.
void print(Document &doc, QPrinter *printer, int rotation);

// Page size after view rotation.
QSizeF rotatedSize(QSizeF size, int rotation);
// Maps page points into the rotated page box.
void applyRotation(QPainter *painter, QSizeF pageSize, int rotation);

}  // namespace Exporter
