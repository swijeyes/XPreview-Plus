// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "ThumbnailSidebar.h"

#include "RenderService.h"

#include <QGuiApplication>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QStyledItemDelegate>

namespace {

constexpr int kLabelHeight = 18;
constexpr int kPad = 8;

class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(ThumbnailModel *model, QObject *parent)
        : QStyledItemDelegate(parent), m_model(model) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        const QSize t = m_model->thumbSize(index.row());
        return QSize(t.width() + 2 * kPad, t.height() + kLabelHeight + 2 * kPad);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize t = m_model->thumbSize(index.row());
        const QRect thumb(opt.rect.x() + (opt.rect.width() - t.width()) / 2, opt.rect.y() + kPad, t.width(),
                          t.height());
        const bool selected = opt.state & QStyle::State_Selected;
        if (selected) {
            p->setPen(QPen(opt.palette.color(QPalette::Highlight), 3));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(QRectF(thumb).adjusted(-4.5, -4.5, 4.5, 4.5), 4, 4);
        }
        p->fillRect(thumb.adjusted(0, 0, 1, 1), QColor(0, 0, 0, 50));
        p->fillRect(thumb, Qt::white);
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        if (!img.isNull()) p->drawImage(thumb, img);

        const QRect label(opt.rect.x(), thumb.bottom() + 4, opt.rect.width(), kLabelHeight);
        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() * 0.85);
        p->setFont(f);
        if (selected) {
            const QString text = index.data(Qt::DisplayRole).toString();
            const int w = QFontMetrics(f).horizontalAdvance(text) + 12;
            const QRect pill(label.center().x() - w / 2, label.y() + 1, w, label.height() - 2);
            p->setPen(Qt::NoPen);
            p->setBrush(opt.palette.color(QPalette::Highlight));
            p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p->setPen(opt.palette.color(QPalette::HighlightedText));
        } else {
            p->setPen(opt.palette.color(QPalette::Text));
        }
        p->drawText(label, Qt::AlignCenter, index.data(Qt::DisplayRole).toString());
        p->restore();
    }

private:
    ThumbnailModel *m_model;
};

}  // namespace

ThumbnailModel::ThumbnailModel(DocumentPtr doc, RenderService *renderer, QObject *parent)
    : QAbstractListModel(parent), m_doc(std::move(doc)), m_renderer(renderer)
{
    connect(renderer, &RenderService::thumbnailReady, this, [this](int page) {
        const QModelIndex i = index(page);
        emit dataChanged(i, i, {Qt::DecorationRole});
    });
}

int ThumbnailModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_doc->pageCount();
}

QSize ThumbnailModel::thumbSize(int page) const
{
    const QSizeF s = m_doc->pageSize(page);
    const double h = m_width * s.height() / qMax(1.0, s.width());
    return QSize(m_width, qBound(16, qRound(h), m_width * 4));
}

void ThumbnailModel::setThumbWidth(int w)
{
    if (w == m_width) return;
    beginResetModel();
    m_width = w;
    endResetModel();
}

QVariant ThumbnailModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return {};
    if (role == Qt::DisplayRole) return QString::number(index.row() + 1);
    if (role == Qt::DecorationRole && m_renderer) {
        // Render at device resolution for crisp thumbnails on Retina.
        const int px = qRound(m_width * qApp->devicePixelRatio());
        return m_renderer->thumbnail(index.row(), px);
    }
    if (role == Qt::ToolTipRole) return tr("Page %1").arg(index.row() + 1);
    return {};
}

ThumbnailSidebar::ThumbnailSidebar(DocumentPtr doc, RenderService *renderer, QWidget *parent)
    : QListView(parent), m_model(new ThumbnailModel(std::move(doc), renderer, this))
{
    setModel(m_model);
    setItemDelegate(new ThumbnailDelegate(m_model, this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setUniformItemSizes(false);
    setLayoutMode(QListView::Batched);
    setBatchSize(50);
    verticalScrollBar()->setSingleStep(20);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    // Width changes are applied after the resize settles, never from inside
    // resizeEvent (relayout there can resize the viewport and re-enter).
    m_widthTimer.setSingleShot(true);
    m_widthTimer.setInterval(80);
    connect(&m_widthTimer, &QTimer::timeout, this, &ThumbnailSidebar::applyThumbWidth);
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &cur) {
        if (!m_syncing && cur.isValid()) emit pageActivated(cur.row());
    });
}

void ThumbnailSidebar::setCurrentPage(int page)
{
    m_syncing = true;
    const QModelIndex i = m_model->index(page);
    setCurrentIndex(i);
    scrollTo(i, QAbstractItemView::EnsureVisible);
    m_syncing = false;
}

int ThumbnailSidebar::targetThumbWidth() const
{
    // Based on the widget's own width with the scrollbar's space always
    // reserved, so showing or hiding the scrollbar cannot change the result.
    // (Using the viewport width fed back forever with classic scrollbars:
    // wider thumbnails -> scrollbar -> narrower viewport -> narrower ...)
    const int scrollbar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
    const int avail = contentsRect().width() - scrollbar - 2 * kPad - 8;
    return qBound(60, avail / 20 * 20, 400);  // steps limit re-rendering
}

void ThumbnailSidebar::applyThumbWidth()
{
    const int w = targetThumbWidth();
    if (w == m_model->thumbWidth()) return;
    const int page = currentIndex().row();
    m_model->setThumbWidth(w);
    if (page >= 0) setCurrentPage(page);
}

void ThumbnailSidebar::resizeEvent(QResizeEvent *e)
{
    QListView::resizeEvent(e);
    if (targetThumbWidth() != m_model->thumbWidth()) m_widthTimer.start();
}
