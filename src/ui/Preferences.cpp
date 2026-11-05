// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "Preferences.h"

#include <QDateTime>
#include <QSettings>

namespace {
const QString kScaleKey = QStringLiteral("pdf/scale");
const QString kRestoreKey = QStringLiteral("pdf/restoreLastPage");
const QString kDisplayKey = QStringLiteral("pdf/displayMode");
const QString kLastPagesKey = QStringLiteral("pdf/lastPages");
const QString kImageScaleKey = QStringLiteral("image/scale");
const QString kWindowModeKey = QStringLiteral("window/mode");
const QString kScrollbarStyleKey = QStringLiteral("window/scrollbarStyle");
constexpr int kMaxRememberedFiles = 200;
}

Preferences *Preferences::instance()
{
    static Preferences *p = new Preferences;  // lives until exit, like qApp
    return p;
}

Preferences::PdfScale Preferences::pdfScale() const
{
    return QSettings().value(kScaleKey).toString() == QLatin1String("pixel") ? PdfScale::PointToPixel
                                                                            : PdfScale::RealSize;
}

void Preferences::setPdfScale(PdfScale scale)
{
    if (scale == pdfScale()) return;
    QSettings().setValue(kScaleKey, scale == PdfScale::PointToPixel ? QStringLiteral("pixel") : QStringLiteral("real"));
    emit pdfScaleChanged();
}

bool Preferences::restoreLastPage() const { return QSettings().value(kRestoreKey, true).toBool(); }

void Preferences::setRestoreLastPage(bool on) { QSettings().setValue(kRestoreKey, on); }

DocumentView::DisplayMode Preferences::pdfDisplayMode() const
{
    const QString v = QSettings().value(kDisplayKey).toString();
    if (v == QLatin1String("single")) return DocumentView::DisplayMode::SinglePage;
    if (v == QLatin1String("two")) return DocumentView::DisplayMode::TwoPages;
    return DocumentView::DisplayMode::Continuous;
}

void Preferences::setPdfDisplayMode(DocumentView::DisplayMode mode)
{
    QSettings().setValue(kDisplayKey, mode == DocumentView::DisplayMode::SinglePage ? QStringLiteral("single")
                                      : mode == DocumentView::DisplayMode::TwoPages ? QStringLiteral("two")
                                                                                    : QStringLiteral("continuous"));
}

Preferences::ImageScale Preferences::imageScale() const
{
    // Default honours the image's own DPI (printed size), so a 300-dpi photo
    // opens at its real size; "pixel" is the opt-in one-pixel-per-point mode.
    return QSettings().value(kImageScaleKey).toString() == QLatin1String("pixel") ? ImageScale::PointToPixel
                                                                                  : ImageScale::RealSize;
}

void Preferences::setImageScale(ImageScale scale)
{
    if (scale == imageScale()) return;
    QSettings().setValue(kImageScaleKey,
                         scale == ImageScale::RealSize ? QStringLiteral("real") : QStringLiteral("pixel"));
    emit imageScaleChanged();
}

Preferences::WindowMode Preferences::windowMode() const
{
    return QSettings().value(kWindowModeKey).toString() == QLatin1String("separate") ? WindowMode::Separate
                                                                                     : WindowMode::Group;
}

void Preferences::setWindowMode(WindowMode mode)
{
    QSettings().setValue(kWindowModeKey,
                         mode == WindowMode::Separate ? QStringLiteral("separate") : QStringLiteral("group"));
}

int Preferences::scrollbarStyle() const
{
    return qBound(1, QSettings().value(kScrollbarStyleKey, 2).toInt(), 4);  // default: Light
}

void Preferences::setScrollbarStyle(int weight)
{
    QSettings().setValue(kScrollbarStyleKey, qBound(1, weight, 4));
}

int Preferences::lastPage(const QString &path)
{
    const QVariantMap pages = QSettings().value(kLastPagesKey).toMap();
    return pages.value(path).toList().value(0, 0).toInt();
}

void Preferences::setLastPage(const QString &path, int page)
{
    QSettings s;
    QVariantMap pages = s.value(kLastPagesKey).toMap();
    // [page, last used]; the oldest entries are dropped.
    pages.insert(path, QVariantList{page, QDateTime::currentSecsSinceEpoch()});
    while (pages.size() > kMaxRememberedFiles) {
        auto oldest = pages.begin();
        for (auto it = pages.begin(); it != pages.end(); ++it)
            if (it->toList().value(1).toLongLong() < oldest->toList().value(1).toLongLong()) oldest = it;
        pages.erase(oldest);
    }
    s.setValue(kLastPagesKey, pages);
}
