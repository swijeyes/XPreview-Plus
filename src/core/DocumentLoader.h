// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "Document.h"
#include "FormatSniffer.h"

struct LoadResult {
    DocumentPtr document;
    QString error;
    bool needsPassword = false;
    SniffResult sniff;
    qint64 elapsedMs = 0;
};

// Sniffs the file and opens it with the matching backend. Safe to call from
// a worker thread; never throws.
LoadResult loadDocument(const QString &path, const QByteArray &password = {});

// File dialog filter covering every supported format.
QString supportedFileFilter();
