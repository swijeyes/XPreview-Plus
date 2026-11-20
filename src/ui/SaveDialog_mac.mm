// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "SaveDialog.h"

#include <QFileInfo>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace SaveDialog {

QString getSavePdfFileName(QWidget *parent, const QString &title, const QString &suggested, bool *protect,
                           bool canProtect, const QString &disabledReason)
{
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = title.toNSString();
        panel.allowedContentTypes = @[ UTTypePDF ];
        panel.canCreateDirectories = YES;
        const QFileInfo fi(suggested);
        panel.directoryURL = [NSURL fileURLWithPath:fi.absolutePath().toNSString() isDirectory:YES];
        panel.nameFieldStringValue = fi.fileName().toNSString();

        // The check box sits in the panel's accessory area, under the file list.
        NSButton *box = [NSButton checkboxWithTitle:QObject::tr("Protect with password").toNSString()
                                             target:nil
                                             action:nil];
        box.state = *protect ? NSControlStateValueOn : NSControlStateValueOff;
        box.enabled = canProtect;
        if (!canProtect) box.toolTip = disabledReason.toNSString();
        [box sizeToFit];
        NSView *accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth(box.frame) + 40, NSHeight(box.frame) + 20)];
        box.frameOrigin = NSMakePoint(20, 10);
        [accessory addSubview:box];
        panel.accessoryView = accessory;

        // App-modal, like Qt's own save dialog; the parent window regains focus after.
        const NSModalResponse response = [panel runModal];
        if (parent) parent->activateWindow();
        if (response != NSModalResponseOK || !panel.URL) return {};
        *protect = box.state == NSControlStateValueOn;
        QString path = QString::fromNSString(panel.URL.path);
        if (!path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) path += QStringLiteral(".pdf");
        return path;
    }
}

}  // namespace SaveDialog
