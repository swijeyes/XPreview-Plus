// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PreferencesDialog.h"

#include "Appearance.h"
#include "AppStyle.h"
#include "Preferences.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QRadioButton>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// A short horizontal scroller thumb in the given theme, for the colour swatch.
QIcon themeSwatch(Appearance::Theme theme)
{
    QIcon icon;
    for (int scale : {1, 2}) {
        QPixmap pm(QSize(36, 14) * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        AppStyle::paintThumb(&p, QRectF(0, 0, 36, 14), Qt::Horizontal, theme);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// A small preview of a flat scroll bar at a given arrow weight (1..4), in the
// current Modern mode (light or dark). Mirrors the weights AppStyle paints.
QIcon scrollbarSwatch(int weight, bool dark)
{
    struct W { QColor fill, glyph; };
    static const W light[4] = {{QColor(0xdc, 0xeb, 0xff), QColor(0x0a, 0x84, 0xff)},
                               {QColor(0xb6, 0xda, 0xff), QColor(0x0a, 0x84, 0xff)},
                               {QColor(0x78, 0xbb, 0xff), QColor(0xff, 0xff, 0xff)},
                               {QColor(0x0a, 0x84, 0xff), QColor(0xff, 0xff, 0xff)}};
    static const W dark_[4] = {{QColor(0x36, 0x36, 0x38), QColor(0xe9, 0xe9, 0xee)},
                               {QColor(0x50, 0x50, 0x53), QColor(0xe9, 0xe9, 0xee)},
                               {QColor(0x7b, 0x7b, 0x7f), QColor(0x1c, 0x1c, 0x1e)},
                               {QColor(0xc9, 0xc9, 0xcf), QColor(0x1c, 0x1c, 0x1e)}};
    const W &w = (dark ? dark_ : light)[qBound(1, weight, 4) - 1];
    const QColor track = dark ? QColor(0x20, 0x20, 0x22) : QColor(0xee, 0xf0, 0xf3);
    const QColor thumb = dark ? QColor(0x48, 0x48, 0x4a) : QColor(0xc2, 0xc2, 0xc8);
    const QColor frame = dark ? QColor(0x3a, 0x3a, 0x3c) : QColor(0xd2, 0xd2, 0xd7);
    const QColor div = dark ? QColor(255, 255, 255, 42) : QColor(0, 0, 0, 36);

    QIcon icon;
    const QSize size(24, 70);
    for (int s : {1, 2}) {
        QPixmap pm(size * s);
        pm.setDevicePixelRatio(s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF body(0.5, 0.5, size.width() - 1, size.height() - 1);
        p.setPen(QPen(frame, 1));
        p.setBrush(track);
        p.drawRoundedRect(body, 3, 3);
        // Thumb near the top.
        p.setPen(Qt::NoPen);
        p.setBrush(thumb);
        p.drawRoundedRect(QRectF(4, 6, size.width() - 8, 22), 5, 5);
        // Two arrow buttons at the bottom.
        const double btnH = 15, top = size.height() - btnH * 2;
        p.setBrush(w.fill);
        p.drawRect(QRectF(1, top, size.width() - 2, btnH * 2 - 1));
        p.setPen(QPen(div, 1));
        p.drawLine(QPointF(1, top + btnH), QPointF(size.width() - 1, top + btnH));
        p.setPen(Qt::NoPen);
        p.setBrush(w.glyph);
        const double cx = size.width() / 2.0, h = 3.0;
        for (int b = 0; b < 2; ++b) {
            const double cy = top + btnH / 2 + b * btnH;
            QPolygonF tri;
            if (b == 0) tri << QPointF(cx, cy - h) << QPointF(cx - 4, cy + h) << QPointF(cx + 4, cy + h);
            else tri << QPointF(cx, cy + h) << QPointF(cx - 4, cy - h) << QPointF(cx + 4, cy - h);
            p.drawPolygon(tri);
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QLabel *makeNote(const QString &text, QWidget *parent)
{
    auto *note = new QLabel(text, parent);
    QFont small = note->font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    note->setFont(small);
    note->setEnabled(false);
    note->setWordWrap(true);
    return note;
}

}  // namespace

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->addTab(buildGeneralTab(), tr("General"));
    tabs->addTab(buildPdfTab(), tr("PDF"));
    tabs->addTab(buildImagesTab(), tr("Images"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);
}

QWidget *PreferencesDialog::buildGeneralTab()
{
    auto *page = new QWidget(this);
    Appearance *a = Appearance::instance();
    Preferences *prefs = Preferences::instance();

    // Top-level Classic / Modern.
    auto *classic = new QRadioButton(tr("Classic"), page);
    auto *modern = new QRadioButton(tr("Modern"), page);
    (a->kind() == Appearance::Kind::Modern ? modern : classic)->setChecked(true);
    auto *kindGroup = new QButtonGroup(page);
    kindGroup->addButton(classic);
    kindGroup->addButton(modern);
    auto *kindRow = new QHBoxLayout;
    kindRow->setSpacing(18);
    kindRow->addWidget(classic);
    kindRow->addWidget(modern);
    kindRow->addStretch();

    // Classic sub-choice: Blue / Gray, each with a scroller swatch.
    auto *blue = new QRadioButton(tr("Blue"), page);
    auto *gray = new QRadioButton(tr("Gray"), page);
    for (auto [button, theme] : {std::pair{blue, Appearance::Theme::Blue}, {gray, Appearance::Theme::Gray}}) {
        button->setIcon(themeSwatch(theme));
        button->setIconSize(QSize(36, 14));
    }
    (a->classicColor() == Appearance::Theme::Gray ? gray : blue)->setChecked(true);
    auto *colorGroup = new QButtonGroup(page);
    colorGroup->addButton(blue);
    colorGroup->addButton(gray);
    auto *classicBox = new QWidget(page);
    auto *classicRow = new QHBoxLayout(classicBox);
    classicRow->setContentsMargins(16, 0, 0, 0);
    classicRow->setSpacing(18);
    classicRow->addWidget(blue);
    classicRow->addWidget(gray);
    classicRow->addStretch();

    // Modern sub-choice: Automatic / Light / Dark.
    auto *modeBox = new QWidget(page);
    auto *modeRow = new QHBoxLayout(modeBox);
    modeRow->setContentsMargins(16, 0, 0, 0);
    auto *mode = new QComboBox(modeBox);
    mode->addItem(tr("Automatic (match system)"), int(Appearance::Mode::Auto));
    mode->addItem(tr("Light"), int(Appearance::Mode::Light));
    mode->addItem(tr("Dark"), int(Appearance::Mode::Dark));
    mode->setCurrentIndex(mode->findData(int(a->mode())));
    modeRow->addWidget(mode);
    modeRow->addStretch();

    // Modern sub-choice: scroll-bar arrow weight, shown as live previews.
    auto *scrollBox = new QWidget(page);
    auto *scrollRow = new QHBoxLayout(scrollBox);
    scrollRow->setContentsMargins(16, 2, 0, 0);
    scrollRow->setSpacing(8);
    auto *scrollGroup = new QButtonGroup(page);
    scrollGroup->setExclusive(true);
    const QString caps[4] = {tr("Lightest"), tr("Light"), tr("Medium"), tr("Solid")};
    QList<QToolButton *> scrollTiles;
    for (int wgt = 1; wgt <= 4; ++wgt) {
        auto *tile = new QToolButton(scrollBox);
        tile->setCheckable(true);
        tile->setAutoRaise(true);
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setIconSize(QSize(24, 70));
        tile->setText(caps[wgt - 1]);
        tile->setChecked(prefs->scrollbarStyle() == wgt);
        scrollGroup->addButton(tile, wgt);
        scrollRow->addWidget(tile);
        scrollTiles << tile;
    }
    scrollRow->addStretch();
    auto refreshScrollSwatches = [scrollTiles] {
        const bool dark = Appearance::instance()->isDark();
        for (int i = 0; i < scrollTiles.size(); ++i) scrollTiles[i]->setIcon(scrollbarSwatch(i + 1, dark));
    };
    refreshScrollSwatches();
    connect(scrollGroup, &QButtonGroup::idToggled, this, [prefs](int id, bool on) {
        if (!on) return;
        prefs->setScrollbarStyle(id);
        for (QWidget *w : QApplication::allWidgets()) w->update();
    });
    connect(Appearance::instance(), &Appearance::themeChanged, this,
            [refreshScrollSwatches](Appearance::Theme) { refreshScrollSwatches(); });

    auto *choices = new QVBoxLayout;
    choices->setSpacing(6);
    choices->addLayout(kindRow);
    choices->addWidget(classicBox);
    choices->addWidget(modeBox);
    choices->addWidget(scrollBox);
    choices->addWidget(makeNote(tr("Sets the window chrome, scroll bars, selection colour and the sidebar."), page));

    auto showSubChoice = [classicBox, modeBox, scrollBox](bool modernOn) {
        classicBox->setVisible(!modernOn);
        modeBox->setVisible(modernOn);
        scrollBox->setVisible(modernOn);
    };
    showSubChoice(a->kind() == Appearance::Kind::Modern);

    connect(classic, &QRadioButton::toggled, this, [a, showSubChoice](bool on) {
        if (on) {
            a->setKind(Appearance::Kind::Classic);
            showSubChoice(false);
        }
    });
    connect(modern, &QRadioButton::toggled, this, [a, showSubChoice](bool on) {
        if (on) {
            a->setKind(Appearance::Kind::Modern);
            showSubChoice(true);
        }
    });
    connect(blue, &QRadioButton::toggled, this, [a](bool on) { if (on) a->setClassicColor(Appearance::Theme::Blue); });
    connect(gray, &QRadioButton::toggled, this, [a](bool on) { if (on) a->setClassicColor(Appearance::Theme::Gray); });
    connect(mode, &QComboBox::currentIndexChanged, this, [a, mode] {
        a->setMode(Appearance::Mode(mode->currentData().toInt()));
    });

    // Opening several files: one window, or a window each.
    auto *together = new QRadioButton(tr("Keep the set together in one window"), page);
    auto *separate = new QRadioButton(tr("Give each file its own window"), page);
    (prefs->windowMode() == Preferences::WindowMode::Separate ? separate : together)->setChecked(true);
    auto *windowGroup = new QButtonGroup(page);
    windowGroup->addButton(together);
    windowGroup->addButton(separate);
    connect(together, &QRadioButton::toggled, this, [prefs](bool on) {
        prefs->setWindowMode(on ? Preferences::WindowMode::Group : Preferences::WindowMode::Separate);
    });
    auto *windowChoices = new QVBoxLayout;
    windowChoices->setSpacing(6);
    windowChoices->addWidget(together);
    windowChoices->addWidget(separate);

    auto *form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    form->addRow(tr("Appearance:"), choices);
    form->addRow(tr("Opening several files:"), windowChoices);
    return page;
}

QWidget *PreferencesDialog::buildPdfTab()
{
    auto *page = new QWidget(this);
    Preferences *prefs = Preferences::instance();

    auto *realSize = new QRadioButton(tr("Pages at their printed size"), page);
    auto *pointPixel = new QRadioButton(tr("One PDF point per screen pixel"), page);
    (prefs->pdfScale() == Preferences::PdfScale::PointToPixel ? pointPixel : realSize)->setChecked(true);
    auto *scaleGroup = new QButtonGroup(page);
    scaleGroup->addButton(realSize);
    scaleGroup->addButton(pointPixel);
    connect(realSize, &QRadioButton::toggled, this, [prefs](bool on) {
        prefs->setPdfScale(on ? Preferences::PdfScale::RealSize : Preferences::PdfScale::PointToPixel);
    });
    auto *scaleChoices = new QVBoxLayout;
    scaleChoices->setSpacing(6);
    scaleChoices->addWidget(realSize);
    scaleChoices->addWidget(pointPixel);

    auto *restore = new QCheckBox(tr("Return to the page I was reading"), page);
    restore->setChecked(prefs->restoreLastPage());
    connect(restore, &QCheckBox::toggled, this, [prefs](bool on) { prefs->setRestoreLastPage(on); });

    auto *display = new QComboBox(page);
    display->addItem(tr("Scrolling pages"), int(DocumentView::DisplayMode::Continuous));
    display->addItem(tr("One page at a time"), int(DocumentView::DisplayMode::SinglePage));
    display->addItem(tr("Side-by-side pages"), int(DocumentView::DisplayMode::TwoPages));
    display->setCurrentIndex(display->findData(int(prefs->pdfDisplayMode())));
    connect(display, &QComboBox::currentIndexChanged, this, [prefs, display] {
        prefs->setPdfDisplayMode(DocumentView::DisplayMode(display->currentData().toInt()));
    });

    auto *form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    form->addRow(tr("Actual size (100%) shows:"), scaleChoices);
    form->addRow(tr("On open:"), restore);
    form->addRow(tr("Initial layout:"), display);
    return page;
}

QWidget *PreferencesDialog::buildImagesTab()
{
    auto *page = new QWidget(this);
    Preferences *prefs = Preferences::instance();

    auto *perPixel = new QRadioButton(tr("One image pixel per screen pixel"), page);
    auto *printed = new QRadioButton(tr("Images at their printed size"), page);
    (prefs->imageScale() == Preferences::ImageScale::RealSize ? printed : perPixel)->setChecked(true);
    auto *scaleGroup = new QButtonGroup(page);
    scaleGroup->addButton(perPixel);
    scaleGroup->addButton(printed);
    connect(perPixel, &QRadioButton::toggled, this, [prefs](bool on) {
        prefs->setImageScale(on ? Preferences::ImageScale::PointToPixel : Preferences::ImageScale::RealSize);
    });
    auto *scaleChoices = new QVBoxLayout;
    scaleChoices->setSpacing(6);
    scaleChoices->addWidget(perPixel);
    scaleChoices->addWidget(printed);
    scaleChoices->addWidget(makeNote(tr("Printed size uses the resolution (DPI) saved in the image."), page));

    auto *form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    form->addRow(tr("Actual size (100%) shows:"), scaleChoices);
    return page;
}
