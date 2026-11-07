// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "RenderService.h"

#include "core/Limits.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>

#include <atomic>

namespace {

QThreadPool *renderPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(qMax(2, QThread::idealThreadCount() - 1));
        p->setStackSize(Limits::kWorkerStackBytes);
        p->setExpiryTimeout(30000);
        return p;
    }();
    return pool;
}

std::atomic<bool> g_shuttingDown{false};

QString tileId(int page, int scaleKey, int tx, int ty)
{
    return QStringLiteral("%1/%2/%3/%4").arg(page).arg(scaleKey).arg(tx).arg(ty);
}

}  // namespace

// State shared with worker jobs; jobs hold a weak reference so a closed
// tab never receives results.
struct RenderService::Shared {
    std::atomic<int> generation{0};
    int structure = 0;  // GUI thread only; see reset()
    QPointer<RenderService> service;
};

RenderService::RenderService(DocumentPtr doc, QObject *parent)
    : QObject(parent), m_doc(std::move(doc)), m_shared(std::make_shared<Shared>())
{
    m_shared->service = this;
    m_tiles.setMaxCost(256 * 1024);  // KB
    m_thumbs.setMaxCost(48 * 1024);
}

void RenderService::shutdownAll()
{
    g_shuttingDown = true;
    renderPool()->clear();
    renderPool()->waitForDone();
}

RenderService::~RenderService()
{
    m_shared->generation++;
    m_shared->service = nullptr;
}

void RenderService::cancelPending()
{
    m_shared->generation++;
    m_pending.clear();
}

void RenderService::invalidatePage(int page)
{
    const QString tiles = QStringLiteral("%1/").arg(page), thumbs = QStringLiteral("t%1/").arg(page);
    for (const QString &id : m_tiles.keys())
        if (page < 0 || id.startsWith(tiles)) m_stale.insert(id);
    for (const QString &id : m_thumbs.keys())
        if (page < 0 || id.startsWith(thumbs)) m_stale.insert(id);
    if (page < 0) {
        // Every page: results of running jobs are dropped and re-requested.
        m_shared->structure++;
        m_pending.clear();
    } else {
        ++m_pageRevision[page];
    }
}

void RenderService::reset()
{
    m_shared->generation++;
    m_shared->structure++;
    m_tiles.clear();
    m_thumbs.clear();
    m_pending.clear();
    m_stale.clear();
    m_pageRevision.clear();
}

QImage RenderService::tile(int page, double scale, int tx, int ty)
{
    const int sk = scaleKey(scale);
    const QString id = tileId(page, sk, tx, ty);
    QImage cached;
    if (QImage *img = m_tiles.object(id)) {
        if (!m_stale.contains(id)) return *img;
        cached = *img;  // shown until the re-render arrives
    }
    if (m_pending.contains(id) || g_shuttingDown) return cached;
    m_pending.insert(id);

    const QSizeF size = m_doc->pageSize(page);
    const QRect full(0, 0, qCeil(size.width() * scale), qCeil(size.height() * scale));
    const QRect rect = QRect(tx * kTileSize, ty * kTileSize, kTileSize, kTileSize).intersected(full);
    if (rect.isEmpty()) {
        m_pending.remove(id);
        return cached;
    }
    const int gen = m_shared->generation;
    const int structure = m_shared->structure;
    const int revision = m_pageRevision.value(page);
    std::weak_ptr<Shared> weak = m_shared;
    DocumentPtr doc = m_doc;
    renderPool()->start([=] {
        {
            auto s = weak.lock();
            if (!s || s->generation != gen || g_shuttingDown) return;  // stale, or quitting
        }
        QImage img = doc->renderPage(page, scale, rect);
        QMetaObject::invokeMethod(qApp, [weak, id, page, structure, revision, img = std::move(img)] {
            auto s = weak.lock();
            if (!s || !s->service || s->structure != structure) return;
            RenderService *self = s->service;
            self->deliver(self->m_tiles, id, page, revision, img);
            emit self->tileReady(page);
        }, Qt::QueuedConnection);
    });
    return cached;
}

void RenderService::deliver(QCache<QString, QImage> &cache, const QString &id, int page, int revision,
                            const QImage &img)
{
    m_pending.remove(id);
    cache.insert(id, new QImage(img), qMax<qsizetype>(1, img.sizeInBytes() / 1024));
    // Rendered before the page last changed: keep it, but render again.
    if (revision == m_pageRevision.value(page)) m_stale.remove(id);
    else m_stale.insert(id);
}

QImage RenderService::thumbnail(int page, int width)
{
    const QString id = QStringLiteral("t%1/%2").arg(page).arg(width);
    QImage cached;
    if (QImage *img = m_thumbs.object(id)) {
        if (!m_stale.contains(id)) return *img;
        cached = *img;
    }
    if (m_pending.contains(id) || g_shuttingDown) return cached;
    m_pending.insert(id);
    const QSizeF size = m_doc->pageSize(page);
    // Fit within width x (aspect cap * width): a hostile 1 x 14400 pt page
    // must not produce a gigantic thumbnail.
    const double maxH = double(width) * Limits::kMaxThumbnailAspect;
    const double scale = qMin(width / qMax(1.0, size.width()), maxH / qMax(1.0, size.height()));
    const QRect rect(0, 0, qMax(1, qCeil(size.width() * scale)), qMax(1, qCeil(size.height() * scale)));
    const int structure = m_shared->structure;
    const int revision = m_pageRevision.value(page);
    std::weak_ptr<Shared> weak = m_shared;
    DocumentPtr doc = m_doc;
    // Thumbnails are not tied to the zoom generation.
    renderPool()->start([=] {
        if (weak.expired() || g_shuttingDown) return;
        QImage img = doc->renderPage(page, scale, rect);
        QMetaObject::invokeMethod(qApp, [weak, id, page, structure, revision, img = std::move(img)] {
            auto s = weak.lock();
            if (!s || !s->service || s->structure != structure) return;
            RenderService *self = s->service;
            self->deliver(self->m_thumbs, id, page, revision, img);
            emit self->thumbnailReady(page);
        }, Qt::QueuedConnection);
    }, -1);  // lower priority than visible tiles
    return cached;
}
