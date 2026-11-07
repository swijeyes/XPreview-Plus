// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "ThumbnailSidebar.h"

#include "RenderService.h"
#include "core/Limits.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QStyledItemDelegate>

namespace {

constexpr int kLabelHeight = 18;
constexpr int kPad = 8;

constexpr const char *kPdfMime = "application/pdf";

// Drag payload. The PDF of the dragged pages is only built if a drop
// target asks for it (another document); reordering never needs it.
class PagesMimeData : public QMimeData {
public:
    PagesMimeData(QVector<int> pages, std::function<QByteArray(const QVector<int> &)> exporter)
        : m_pages(std::move(pages)), m_exporter(std::move(exporter))
    {
        QByteArray rows;
        for (int p : std::as_const(m_pages)) rows += QByteArray::number(p) + ',';
        setData(QString::fromLatin1(ThumbnailSidebar::kPagesMime), rows);
    }
    QStringList formats() const override
    {
        QStringList f = QMimeData::formats();
        if (m_exporter) f << QString::fromLatin1(kPdfMime);
        return f;
    }
    bool hasFormat(const QString &mime) const override
    {
        return (m_exporter && mime == QLatin1String(kPdfMime)) || QMimeData::hasFormat(mime);
    }

protected:
    QVariant retrieveData(const QString &mime, QMetaType type) const override
    {
        if (mime == QLatin1String(kPdfMime) && m_exporter) {
            if (!m_built) {
                m_pdf = m_exporter(m_pages);
                m_built = true;
            }
            return m_pdf;
        }
        return QMimeData::retrieveData(mime, type);
    }

private:
    QVector<int> m_pages;
    std::function<QByteArray(const QVector<int> &)> m_exporter;
    mutable QByteArray m_pdf;
    mutable bool m_built = false;
};

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

void ThumbnailModel::reload()
{
    beginResetModel();
    endResetModel();
}

Qt::ItemFlags ThumbnailModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (m_editable && index.isValid()) f |= Qt::ItemIsDragEnabled;
    return f;
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
        // Render at device resolution for crisp thumbnails on high-DPI displays.
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
    // Several pages can be selected (Shift/Cmd-click) to move, rotate or
    // delete them together.
    setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    // Scrolling through a multi-page selection keeps it.
    if (selectionModel()->isSelected(i)) selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
    else setCurrentIndex(i);
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

void ThumbnailSidebar::setEditable(bool on)
{
    m_model->setEditable(on);
    setDragEnabled(on);
    setAcceptDrops(on);
    viewport()->setAcceptDrops(on);  // drag events are delivered to the viewport
    setDragDropMode(on ? QAbstractItemView::DragDrop : QAbstractItemView::NoDragDrop);
    setDropIndicatorShown(false);  // drawn by paintEvent() between thumbnails
}

void ThumbnailSidebar::reload(const QVector<int> &select)
{
    m_syncing = true;
    m_model->reload();
    const int n = m_model->rowCount();
    QItemSelection sel;
    for (int p : select)
        if (p >= 0 && p < n) sel.select(m_model->index(p), m_model->index(p));
    selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    if (!select.isEmpty() && select.first() >= 0 && select.first() < n) {
        selectionModel()->setCurrentIndex(m_model->index(select.first()), QItemSelectionModel::NoUpdate);
        scrollTo(m_model->index(select.first()), QAbstractItemView::EnsureVisible);
    }
    m_syncing = false;
}

QVector<int> ThumbnailSidebar::selectedPages() const
{
    QVector<int> pages;
    for (const QModelIndex &i : selectionModel()->selectedIndexes()) pages << i.row();
    std::sort(pages.begin(), pages.end());
    return pages;
}

void ThumbnailSidebar::startDrag(Qt::DropActions)
{
    const QVector<int> pages = selectedPages();
    if (pages.isEmpty()) return;
    auto *drag = new QDrag(this);
    drag->setMimeData(new PagesMimeData(pages, pageExporter));
    // The first page's thumbnail, with a count for several pages.
    QImage img = m_model->data(m_model->index(pages.first()), Qt::DecorationRole).value<QImage>();
    if (!img.isNull()) {
        const qreal dpr = devicePixelRatioF();
        QPixmap pm = QPixmap::fromImage(img.scaledToWidth(qRound(72 * dpr), Qt::SmoothTransformation));
        pm.setDevicePixelRatio(dpr);
        if (pages.size() > 1) {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            const QString n = QString::number(pages.size());
            QFont f = font();
            f.setBold(true);
            p.setFont(f);
            const int w = qMax(20, QFontMetrics(f).horizontalAdvance(n) + 10);
            const QRectF badge(pm.width() / dpr - w - 2, 2, w, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xff, 0x3b, 0x30));
            p.drawRoundedRect(badge, 10, 10);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, n);
        }
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(qRound(pm.width() / dpr / 2), 10));
    }
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
}

bool ThumbnailSidebar::acceptsDrop(const QMimeData *mime) const
{
    if (!isEditable()) return false;
    if (mime->hasFormat(QString::fromLatin1(kPagesMime)) || mime->hasFormat(QString::fromLatin1(kPdfMime)))
        return true;
    for (const QUrl &url : mime->urls())
        if (url.isLocalFile()) return true;
    return false;
}

int ThumbnailSidebar::dropIndex(QPoint pos) const
{
    const int n = m_model->rowCount();
    if (const QModelIndex i = indexAt(pos); i.isValid())
        return pos.y() < visualRect(i).center().y() ? i.row() : i.row() + 1;
    for (int r = 0; r < n; ++r)
        if (pos.y() < visualRect(m_model->index(r)).center().y()) return r;
    return n;
}

void ThumbnailSidebar::setDropIndex(int index)
{
    if (index == m_dropIndex) return;
    m_dropIndex = index;
    viewport()->update();
}

void ThumbnailSidebar::dragEnterEvent(QDragEnterEvent *e)
{
    // Not ours (e.g. a non-PDF document): the window opens dropped files.
    if (!acceptsDrop(e->mimeData())) return e->ignore();
    const bool move = e->source() == this && !(e->modifiers() & Qt::AltModifier);
    e->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    setDropIndex(dropIndex(e->position().toPoint()));
}

void ThumbnailSidebar::dragMoveEvent(QDragMoveEvent *e)
{
    if (!acceptsDrop(e->mimeData())) return e->ignore();
    const QPoint pos = e->position().toPoint();
    const bool move = e->source() == this && !(e->modifiers() & Qt::AltModifier);
    e->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    // Scroll when the drag nears either end.
    const int edge = 30;
    QScrollBar *v = verticalScrollBar();
    if (pos.y() < edge) v->setValue(v->value() - 12);
    else if (pos.y() > viewport()->height() - edge) v->setValue(v->value() + 12);
    setDropIndex(dropIndex(pos));
}

void ThumbnailSidebar::dragLeaveEvent(QDragLeaveEvent *e)
{
    setDropIndex(-1);
    e->accept();
}

void ThumbnailSidebar::dropEvent(QDropEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!acceptsDrop(mime)) return e->ignore();
    const int before = dropIndex(e->position().toPoint());
    setDropIndex(-1);
    const bool own = e->source() == this;
    const bool copy = e->modifiers() & Qt::AltModifier;
    // Changes are made once the drag has finished, outside its event loop.
    auto later = [this](auto fn) { QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection); };
    if (own && !copy) {
        QVector<int> pages;
        for (const QByteArray &row : mime->data(QString::fromLatin1(kPagesMime)).split(','))
            if (!row.isEmpty()) pages << row.toInt();
        e->setDropAction(Qt::MoveAction);
        e->accept();
        later([this, pages, before] { emit movePagesRequested(pages, before); });
        return;
    }
    if (mime->hasFormat(QString::fromLatin1(kPdfMime))) {
        const QByteArray pdf = mime->data(QString::fromLatin1(kPdfMime));
        e->setDropAction(Qt::CopyAction);
        e->accept();
        if (!pdf.isEmpty() && pdf.size() <= Limits::kMaxInsertPdfBytes) later([this, pdf, before] { emit pdfDropped(pdf, before); });
        return;
    }
    QStringList paths;
    for (const QUrl &url : mime->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    e->setDropAction(Qt::CopyAction);
    e->accept();
    later([this, paths, before] { emit filesDropped(paths, before); });
}

void ThumbnailSidebar::paintEvent(QPaintEvent *e)
{
    QListView::paintEvent(e);
    const int n = m_model->rowCount();
    if (m_dropIndex < 0 || n == 0) return;
    // Insertion line between thumbnails.
    const int y = m_dropIndex < n ? visualRect(m_model->index(m_dropIndex)).top()
                                  : visualRect(m_model->index(n - 1)).bottom() + 1;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = palette().color(QPalette::Highlight);
    p.setPen(QPen(c, 3, Qt::SolidLine, Qt::RoundCap));
    const qreal yy = qBound(2, y, viewport()->height() - 2);
    p.drawLine(QPointF(kPad + 6, yy), QPointF(viewport()->width() - kPad, yy));
    p.setBrush(palette().color(QPalette::Base));
    p.setPen(QPen(c, 2));
    p.drawEllipse(QPointF(kPad + 2, yy), 4, 4);
}

void ThumbnailSidebar::keyPressEvent(QKeyEvent *e)
{
    if (isEditable() && (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) &&
        !selectedPages().isEmpty()) {
        emit deleteRequested();
        return;
    }
    QListView::keyPressEvent(e);
}
