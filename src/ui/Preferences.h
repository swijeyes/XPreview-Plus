// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "DocumentView.h"

#include <QObject>

// Application preferences, persisted in QSettings.
class Preferences : public QObject {
    Q_OBJECT
public:
    // What 100% zoom means for PDFs.
    enum class PdfScale {
        RealSize,     // size on screen equals size on printout
        PointToPixel  // 1 point equals 1 screen pixel
    };
    // What 100% zoom means for images.
    enum class ImageScale {
        PointToPixel,  // one image pixel per screen pixel (ignores file DPI)
        RealSize       // images at their printed size (honours file DPI)
    };
    // How a group of files opens.
    enum class WindowMode {
        Group,     // keep the set together in one window (tabs)
        Separate   // give each file its own window
    };

    static Preferences *instance();

    PdfScale pdfScale() const;
    void setPdfScale(PdfScale scale);
    bool restoreLastPage() const;
    void setRestoreLastPage(bool on);
    DocumentView::DisplayMode pdfDisplayMode() const;
    void setPdfDisplayMode(DocumentView::DisplayMode mode);

    ImageScale imageScale() const;
    void setImageScale(ImageScale scale);

    WindowMode windowMode() const;
    void setWindowMode(WindowMode mode);

    // Scroll-bar arrow-button weight for the Modern look, 1 (lightest) to 4
    // (solid). Ignored by the Classic look.
    int scrollbarStyle() const;
    void setScrollbarStyle(int weight);

    // Last viewed page per file (most recent files only).
    static int lastPage(const QString &path);
    static void setLastPage(const QString &path, int page);

signals:
    void pdfScaleChanged();
    void imageScaleChanged();

private:
    Preferences() = default;
};
