// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QCache>
#include <QObject>
#include <QSet>

// Asynchronous, cached page rendering for one document. Pages are rendered
// in square tiles so deep zoom never allocates a whole-page bitmap, plus a
// separate cache of small whole-page thumbnails. Results are delivered on
// the GUI thread through signals.
class RenderService : public QObject {
    Q_OBJECT
public:
    static constexpr int kTileSize = 512;  // device pixels

    explicit RenderService(DocumentPtr doc, QObject *parent = nullptr);
    ~RenderService() override;

    static int scaleKey(double scale) { return qMax(1, qRound(scale * 1000)); }

    // Returns the tile if cached, otherwise schedules it and returns null.
    QImage tile(int page, double scale, int tx, int ty);
    // Whole-page render at a given pixel width (thumbnails, fallbacks).
    QImage thumbnail(int page, int width);
    // Drops queued (not yet started) tile jobs, e.g. after a zoom change.
    void cancelPending();

    // Stops all rendering: drops queued jobs and waits for running ones.
    // Must run before the QApplication is destroyed, or a job still drawing
    // text would use Qt's already-destroyed font/format machinery.
    static void shutdownAll();

signals:
    void tileReady(int page);
    void thumbnailReady(int page);

private:
    struct Shared;
    DocumentPtr m_doc;
    std::shared_ptr<Shared> m_shared;
    QCache<QString, QImage> m_tiles;
    QCache<QString, QImage> m_thumbs;
    QSet<QString> m_pending;
};
