// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

// xpreview-render: headless renderer for golden tests and importer debugging.
//   xpreview-render <file> [--out DIR] [--scale S] [--pdf OUT.pdf] [--pages N]
//                   [--search TEXT] [--rotate DEG]
#include "core/DocumentLoader.h"
#include "core/Exporter.h"
#include "core/FontSubstitution.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    FontSubstitution::loadBundledFonts();
    QStringList args = app.arguments().mid(1);
    QTextStream out(stdout);
    if (args.isEmpty()) {
        out << "usage: xpreview-render <file> [--out DIR] [--scale S] [--pdf OUT] [--pages N] [--search TEXT]\n";
        return 2;
    }
    const QString file = args.takeFirst();
    QString outDir, pdfOut, search;
    double scale = 1.5;
    int maxPages = 3, rotate = 0;
    while (!args.isEmpty()) {
        const QString a = args.takeFirst();
        if (a == "--out" && !args.isEmpty()) outDir = args.takeFirst();
        else if (a == "--scale" && !args.isEmpty()) scale = args.takeFirst().toDouble();
        else if (a == "--pdf" && !args.isEmpty()) pdfOut = args.takeFirst();
        else if (a == "--pages" && !args.isEmpty()) maxPages = args.takeFirst().toInt();
        else if (a == "--search" && !args.isEmpty()) search = args.takeFirst();
        else if (a == "--rotate" && !args.isEmpty()) rotate = args.takeFirst().toInt();
    }

    const LoadResult r = loadDocument(file);
    if (!r.document) {
        out << "ERROR: " << r.error << "\n";
        return 1;
    }
    Document &doc = *r.document;
    out << "kind: " << documentKindName(doc.kind()) << "\n";
    out << "pages: " << doc.pageCount() << "  load: " << r.elapsedMs << " ms\n";
    out << "page 1 size: " << doc.pageSize(0).width() << " x " << doc.pageSize(0).height() << " pt\n";
    for (const QString &l : doc.fidelityReport().lines()) out << "report: " << l << "\n";
    const auto meta = doc.metadata();
    for (auto it = meta.cbegin(); it != meta.cend(); ++it) out << "meta: " << it.key() << " = " << it.value() << "\n";
    std::function<void(const QList<OutlineItem> &, int)> dumpOutline = [&](const QList<OutlineItem> &items, int d) {
        for (const OutlineItem &i : items) {
            out << "outline: " << QString(d * 2, ' ') << i.title << " -> p" << (i.page + 1) << "\n";
            dumpOutline(i.children, d + 1);
        }
    };
    dumpOutline(doc.outline(), 0);

    if (!outDir.isEmpty()) {
        QDir().mkpath(outDir);
        const QString base = QFileInfo(file).fileName().replace(QLatin1Char('.'), QLatin1Char('_'));
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < doc.pageCount() && i < maxPages; ++i) {
            const QSizeF s = doc.pageSize(i);
            const QRect px(0, 0, qCeil(s.width() * scale), qCeil(s.height() * scale));
            QImage img = doc.renderPage(i, scale, px);
            if (!search.isEmpty()) {
                QPainter p(&img);
                for (const TextHit &h : doc.search(search, false))
                    for (const PageRect &b : h.boxes)
                        if (b.page == i)
                            p.fillRect(QRectF(b.rect.topLeft() * scale, b.rect.size() * scale), QColor(255, 200, 0, 110));
            }
            img.save(QStringLiteral("%1/%2-p%3.png").arg(outDir, base).arg(i + 1));
        }
        out << "rendered in " << t.elapsed() << " ms\n";
    }
    if (!search.isEmpty()) {
        const auto hits = doc.search(search, false);
        out << "search '" << search << "': " << hits.size() << " hits";
        if (!hits.isEmpty()) out << " (first on page " << hits.first().page() + 1 << ")";
        out << "\n";
    }
    if (!pdfOut.isEmpty()) {
        QString err;
        const bool ok = Exporter::exportPdf(doc, pdfOut, rotate, &err);
        out << "export pdf: " << (ok ? "ok" : err) << "\n";
        if (!ok) return 1;
    }
    return 0;
}
