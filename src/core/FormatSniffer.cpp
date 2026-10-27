// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "FormatSniffer.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>

namespace {

SniffedFormat formatForExtension(const QString &ext)
{
    static const QStringList images = {"png", "jpg", "jpeg", "gif", "webp", "tif", "tiff",
                                       "bmp", "heic", "heif", "ico", "icns", "tga", "jp2"};
    if (ext == "pdf") return SniffedFormat::Pdf;
    if (ext == "rtf") return SniffedFormat::Rtf;
    if (ext == "odt" || ext == "ott") return SniffedFormat::OdfText;
    if (ext == "ods" || ext == "ots" || ext == "fods") return SniffedFormat::OdfSpreadsheet;
    if (ext == "odp" || ext == "otp" || ext == "fodp") return SniffedFormat::OdfPresentation;
    if (ext == "fodt") return SniffedFormat::FlatOdfText;
    if (ext == "svg" || ext == "svgz") return SniffedFormat::Svg;
    if (images.contains(ext)) return SniffedFormat::Image;
    return SniffedFormat::Unknown;
}

SniffedFormat odfFromMime(const QByteArray &mime, bool flat)
{
    if (mime.startsWith("application/vnd.oasis.opendocument.text"))
        return flat ? SniffedFormat::FlatOdfText : SniffedFormat::OdfText;
    if (mime.startsWith("application/vnd.oasis.opendocument.spreadsheet"))
        return SniffedFormat::OdfSpreadsheet;
    if (mime.startsWith("application/vnd.oasis.opendocument.presentation"))
        return SniffedFormat::OdfPresentation;
    return SniffedFormat::Unknown;
}

SniffedFormat sniffContent(const QByteArray &head)
{
    if (head.left(1024).contains("%PDF-")) return SniffedFormat::Pdf;
    QByteArray trimmed = head;
    if (trimmed.startsWith("\xEF\xBB\xBF")) trimmed.remove(0, 3);
    if (trimmed.startsWith("{\\rtf")) return SniffedFormat::Rtf;

    // ODF zip: local file header, first entry named "mimetype", stored.
    if (head.startsWith("PK\x03\x04") && head.size() > 38 && head.mid(30, 8) == "mimetype") {
        const int nameLen = quint8(head[26]) | (quint8(head[27]) << 8);
        const int extraLen = quint8(head[28]) | (quint8(head[29]) << 8);
        const int dataStart = 30 + nameLen + extraLen;
        const QByteArray mime = head.mid(dataStart, 80);
        const SniffedFormat f = odfFromMime(mime, false);
        if (f != SniffedFormat::Unknown) return f;
    }

    // Flat ODF: XML with office:document and office:mimetype attribute.
    const int pos = head.indexOf("office:mimetype=\"");
    if (pos >= 0 && head.contains("office:document")) {
        const SniffedFormat f = odfFromMime(head.mid(pos + 17, 80), true);
        if (f != SniffedFormat::Unknown) return f;
    }

    if (head.contains("<svg") && (head.startsWith("<?xml") || head.trimmed().startsWith("<svg") ||
                                  head.contains("<!DOCTYPE svg")))
        return SniffedFormat::Svg;
    if (head.startsWith("\x1f\x8b")) return SniffedFormat::Unknown;  // maybe svgz, use ext
    return SniffedFormat::Unknown;
}

}  // namespace

SniffResult sniffFile(const QString &path)
{
    SniffResult r;
    QFile f(path);
    const QString ext = QFileInfo(path).suffix().toLower();
    const SniffedFormat byExt = formatForExtension(ext);
    if (!f.open(QIODevice::ReadOnly)) {
        r.description = QStringLiteral("Cannot read file");
        return r;
    }
    const QByteArray head = f.read(4096);
    f.close();

    SniffedFormat byContent = sniffContent(head);
    if (byContent == SniffedFormat::Unknown) {
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        // SVG must go through the sanitising SVG backend, never the raw
        // image plugin.
        if (reader.canRead())
            byContent = reader.format().startsWith("svg") ? SniffedFormat::Svg : SniffedFormat::Image;
    }
    if (byContent == SniffedFormat::Unknown && byExt == SniffedFormat::Svg)
        byContent = SniffedFormat::Svg;  // compressed svgz

    r.format = byContent != SniffedFormat::Unknown ? byContent : SniffedFormat::Unknown;
    r.extensionMismatch = byContent != SniffedFormat::Unknown && byExt != SniffedFormat::Unknown &&
                          byExt != byContent;
    return r;
}
