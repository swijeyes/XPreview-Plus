// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "core/DocumentLoader.h"
#include "core/Exporter.h"
#include "core/FontSubstitution.h"
#include "core/FormatSniffer.h"
#include "core/Limits.h"
#include "core/SafeImage.h"
#include "flow/FlowDocument.h"
#include "odf/OdfStyles.h"
#include "rtf/RtfImporter.h"
#include "rtf/RtfTokenizer.h"

#include <QTemporaryDir>

#include <limits>
#include <QtTest>

using namespace flow;

class TestImporters : public QObject {
    Q_OBJECT

private:
    static QString allText(const Blocks &blocks)
    {
        QString s;
        for (const Block &b : blocks) {
            if (b.kind == Block::Para) s += b.para.plainText() + QLatin1Char('\n');
            else if (b.table)
                for (const Row &r : b.table->rows)
                    for (const Cell &c : r.cells) s += allText(c.blocks);
        }
        return s;
    }

private slots:
    void initTestCase() { FontSubstitution::loadBundledFonts(); }

    void exportKeepsFontStyles_data()
    {
        QTest::addColumn<QString>("family");
        for (const char *f : {"Times New Roman", "Times", "Arial", "Courier New", "Liberation Serif",
                              "Liberation Sans", "Calibri", "Cambria"})
            QTest::newRow(f) << QString::fromLatin1(f);
    }

    void exportKeepsFontStyles()
    {
        // Regression: Liberation 2.x faces share one unique ID, and Qt's PDF
        // writer merged them, so exported bold/italic text all used one face.
        QFETCH(QString, family);
        flow::Document model;
        Paragraph p;
        for (int style = 0; style < 3; ++style) {
            Run r;
            r.text = QStringLiteral("Style %1 ").arg(style);
            r.style.family = family;
            r.style.bold = style == 1;
            r.style.italic = style == 2;
            p.runs << r;
        }
        model.body << Block::paragraph(p);
        FlowDocument doc(DocumentKind::Rtf, model, QString());
        QTemporaryDir dir;
        const QString out = dir.filePath(QStringLiteral("styles.pdf"));
        QString err;
        QVERIFY2(Exporter::exportPdf(doc, out, 0, &err), qPrintable(err));
        QFile f(out);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray pdf = f.readAll();
        QSet<QByteArray> fonts;
        static const QRegularExpression re(QStringLiteral("/BaseFont\\s*/([A-Za-z0-9+_,-]+)"));
        for (auto it = re.globalMatch(QString::fromLatin1(pdf)); it.hasNext();)
            fonts.insert(it.next().captured(1).toLatin1().split('+').last());
        QVERIFY2(fonts.size() >= 3, qPrintable(QStringLiteral("%1 embedded as: %2")
                                                   .arg(family, QStringList(QList<QString>(fonts.begin(), fonts.end())).join(", "))));
    }

    void tokenizer()
    {
        const QByteArray in = R"({\rtf1\b bold\b0 \'e9\u8364?{\*\x skip}})";
        rtf::Tokenizer t(in);
        QList<rtf::Token::Type> types;
        for (rtf::Token tok = t.next(); tok.type != rtf::Token::End; tok = t.next()) types << tok.type;
        QCOMPARE(types.first(), rtf::Token::GroupStart);
        QVERIFY(types.contains(rtf::Token::HexByte));
        QCOMPARE(types.last(), rtf::Token::GroupEnd);
    }

    void rtfBasics()
    {
        const QByteArray in =
            R"({\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\froman Times New Roman;}{\f1\fswiss Arial;}})"
            R"({\colortbl;\red255\green0\blue0;})"
            R"(\paperw12240\paperh15840\margl1440\margr1440)"
            R"(\pard\qc\b\fs32 Title\b0\par)"
            R"(\pard\f1\fs24 Caf\'e9 \u8364? \cf1 red\cf0  {\i italic}\par})";
        const flow::Document d = rtf::import(in);
        QCOMPARE(d.body.size(), 2);
        QCOMPARE(d.body[0].para.style.align, Qt::AlignHCenter);
        QVERIFY(d.body[0].para.runs[0].style.bold);
        QCOMPARE(d.body[0].para.runs[0].style.size, 16.0);
        const QString t = d.body[1].para.plainText();
        QVERIFY2(t.contains(QStringLiteral("Café €")), qPrintable(t));
        QVERIFY(!t.contains(QLatin1Char('?')));
        bool sawRed = false, sawItalic = false;
        for (const Run &r : d.body[1].para.runs) {
            if (r.text.contains("red")) sawRed = r.style.color == QColor(255, 0, 0);
            if (r.text.contains("italic")) sawItalic = r.style.italic;
        }
        QVERIFY(sawRed);
        QVERIFY(sawItalic);
        QCOMPARE(d.page.size, QSizeF(612, 792));
    }

    void rtfTable()
    {
        const QByteArray in =
            R"({\rtf1\ansi)"
            R"(\trowd\cellx2000\cellx4000\pard\intbl A1\cell B1\cell\row)"
            R"(\trowd\cellx2000\cellx4000\pard\intbl A2\cell B2\cell\row)"
            R"(\pard After\par})";
        const flow::Document d = rtf::import(in);
        QCOMPARE(d.body.size(), 2);
        QCOMPARE(d.body[0].kind, Block::TableBlock);
        const Table &t = *d.body[0].table;
        QCOMPARE(t.rows.size(), 2);
        QCOMPARE(t.columnCount(), 2);
        QCOMPARE(allText(t.rows[1].cells[1].blocks).trimmed(), QStringLiteral("B2"));
        QCOMPARE(d.body[1].para.plainText(), QStringLiteral("After"));
    }

    void rtfListsAndFields()
    {
        const QByteArray in =
            R"({\rtf1\ansi{\*\listtable{\list\listtemplateid1{\listlevel\levelnfc23{\leveltext\'01\u8226 ?;}}\listid7}})"
            R"({\*\listoverridetable{\listoverride\listid7\listoverridecount0\ls1}})"
            R"(\pard\ls1\ilvl0{\listtext \'95\tab}One\par)"
            R"(\pard{\field{\*\fldinst HYPERLINK "https://example.com"}{\fldrslt link}}\par})";
        const flow::Document d = rtf::import(in);
        QCOMPARE(d.body.size(), 2);
        QVERIFY(d.body[0].para.list.has_value());
        QCOMPARE(d.body[0].para.plainText(), QStringLiteral("One"));
        QCOMPARE(d.body[1].para.runs.first().href, QStringLiteral("https://example.com"));
    }

    void rtfMalformedNeverCrashes()
    {
        const QList<QByteArray> inputs = {
            "", "{", "}}}}", "{\\rtf1 {{{{{{ unbalanced", "{\\rtf1\\bin999999999 x}",
            "{\\rtf1\\u-1?\\u65535 \\'zz\\'4}", "{\\rtf1{\\pict\\pngblip 89504e47}}",
            QByteArray(5000, '{'), "{\\rtf1\\trowd\\cellx-5\\cellx0\\cell\\row\\cell}",
        };
        for (const QByteArray &in : inputs) {
            const flow::Document d = rtf::import(in);
            Q_UNUSED(d);
        }
        QVERIFY(true);
    }

    void odfUnits()
    {
        QCOMPARE(odf::toPoints(u"2.54cm"), 72.0);
        QCOMPARE(odf::toPoints(u"1in"), 72.0);
        QCOMPARE(odf::toPoints(u"10pt"), 10.0);
        QCOMPARE(qRound(odf::toPoints(u"25.4mm")), 72);
        bool ok = true;
        odf::toPoints(u"50%", &ok);
        QVERIFY(!ok);
    }

    void flowPaginationAndSearch()
    {
        flow::Document model;
        model.page.size = QSizeF(300, 300);
        model.page.margins = QMarginsF(20, 20, 20, 20);
        for (int i = 0; i < 80; ++i) {
            Paragraph p;
            Run r;
            r.text = QStringLiteral("Paragraph number %1 with needle").arg(i);
            p.runs << r;
            model.body << Block::paragraph(p);
        }
        FlowDocument doc(DocumentKind::Rtf, model, QString());
        QVERIFY(doc.pageCount() > 3);
        const auto hits = doc.search(QStringLiteral("needle"), false);
        QCOMPARE(hits.size(), 80);
        QVERIFY(hits.last().page() > 0);
        for (const TextHit &h : hits)
            for (const PageRect &b : h.boxes) {
                QVERIFY(b.rect.top() >= 0);
                QVERIFY(b.rect.bottom() <= 300);
            }
    }

    void exportFlowToPdf()
    {
        flow::Document model;
        Paragraph p;
        Run r;
        r.text = QStringLiteral("Hello PDF");
        p.runs << r;
        model.body << Block::paragraph(p);
        FlowDocument doc(DocumentKind::Rtf, model, QString());
        QTemporaryDir dir;
        const QString out = dir.filePath(QStringLiteral("out.pdf"));
        QString err;
        QVERIFY2(Exporter::exportPdf(doc, out, 0, &err), qPrintable(err));
        // Round-trip through our own PDF backend.
        const LoadResult lr = loadDocument(out);
        QVERIFY2(lr.document, qPrintable(lr.error));
        QCOMPARE(lr.document->pageCount(), 1);
        QVERIFY(lr.document->pageText(0).contains(QStringLiteral("Hello PDF")));
        QCOMPARE(lr.document->search(QStringLiteral("hello"), false).size(), 1);
    }

    void sniffing()
    {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &data) {
            QFile f(dir.filePath(name));
            if (f.open(QIODevice::WriteOnly)) f.write(data);
            return f.fileName();
        };
        QCOMPARE(sniffFile(write("a.pdf", "%PDF-1.4\n")).format, SniffedFormat::Pdf);
        QCOMPARE(sniffFile(write("a.txt", "{\\rtf1 hi}")).format, SniffedFormat::Rtf);
        const SniffResult mism = sniffFile(write("b.pdf", "{\\rtf1 hi}"));
        QCOMPARE(mism.format, SniffedFormat::Rtf);
        QVERIFY(mism.extensionMismatch);
    }
    // --- Security regressions ------------------------------------------------

    void secTokenizerNoRecursionOnMalformedHex()
    {
        // Each malformed \' used to recurse once: millions overflowed the stack.
        QByteArray in = "{\\rtf1 ";
        for (int i = 0; i < 2'000'000; ++i) in += "\\'zz";
        in += "}";
        const flow::Document d = rtf::import(in);
        QVERIFY(d.body.size() <= 1);
    }

    void secRtfHostileGeometryIsClamped()
    {
        const QByteArray in = "{\\rtf1\\paperw2147483647\\paperh-5\\margl-99999\\margr2147483647 Hi\\par}";
        const flow::Document d = rtf::import(in);
        QVERIFY(d.page.size.width() <= Limits::kMaxPageSide);
        QVERIFY(d.page.size.height() >= Limits::kMinPageSide);
        QVERIFY(d.page.margins.left() >= 0);
        QVERIFY(d.page.margins.right() <= d.page.size.width() / 2);
        FlowDocument doc(DocumentKind::Rtf, d, QString());
        QCOMPARE(doc.pageCount(), 1);
    }

    void secRtfCellEdgeOverflow()
    {
        // Found by UBSan fuzzing: \cellx2147483647 overflowed edge + 1.
        const QByteArray in = "{\\rtf1\\trowd\\trleft-2147483647\\cellx2147483647\\cellx2147483647"
                              "\\pard\\intbl a\\cell b\\cell\\row}";
        const flow::Document d = rtf::import(in);
        QVERIFY(!d.body.isEmpty());
    }

    void secRtfTableCellBudget()
    {
        // 2000 rows x 200 cells = 400k cells > budget: must be truncated.
        QByteArray row = "\\trowd";
        for (int c = 1; c <= 200; ++c) row += "\\cellx" + QByteArray::number(c * 50);
        row += "\\pard\\intbl";
        for (int c = 0; c < 200; ++c) row += "x\\cell ";
        row += "\\row ";
        QByteArray in = "{\\rtf1 ";
        for (int r = 0; r < 2000; ++r) in += row;
        in += "}";
        const flow::Document d = rtf::import(in);
        qint64 cells = 0;
        for (const Block &b : d.body)
            if (b.table) cells += qint64(b.table->rows.size()) * b.table->columnCount();
        QVERIFY(cells <= Limits::kMaxTableCells);
    }

    void secSvgExternalReferencesStripped()
    {
        const QByteArray svg =
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
            "<image xlink:href='file:///etc/passwd'/><image href=\"/Users/x/secret.png\"/>"
            "<use href='#ok'/><image href='data:image/png;base64,AAAA'/></svg>";
        const QString out = QString::fromUtf8(SafeImage::sanitizeSvg(svg));
        QVERIFY(!out.contains("passwd"));
        QVERIFY(!out.contains("secret"));
        QVERIFY(out.contains("#ok"));
        QVERIFY(out.contains("data:image/png"));
    }

    void secEmbeddedImageDimensionsChecked()
    {
        // PNG header claiming 30000 x 30000 with no pixel data.
        QByteArray png("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR", 16);
        auto be32 = [](quint32 v) {
            QByteArray b(4, 0);
            for (int i = 0; i < 4; ++i) b[i] = char((v >> (24 - 8 * i)) & 0xff);
            return b;
        };
        QByteArray ihdr = QByteArray("IHDR") + be32(30000) + be32(30000) + QByteArray("\x08\x06\x00\x00\x00", 5);
        quint32 crc = 0xffffffffu;  // CRC-32 so decoders accept the header
        for (char ch : ihdr) {
            crc ^= quint8(ch);
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        png += ihdr.mid(4) + be32(~crc);
        SafeImage::Budget budget(Limits::kMaxDocumentImageBytes);
        bool refused = false;
        QVERIFY(SafeImage::decodeEmbedded(png, budget, &refused).isNull());
        QVERIFY(refused);
    }

    void secOdfDeepNestingAndLengths()
    {
        QByteArray x = "<?xml version='1.0'?><office:document "
                       "xmlns:office='urn:oasis:names:tc:opendocument:xmlns:office:1.0' "
                       "xmlns:table='urn:oasis:names:tc:opendocument:xmlns:table:1.0' "
                       "xmlns:text='urn:oasis:names:tc:opendocument:xmlns:text:1.0' "
                       "office:mimetype='application/vnd.oasis.opendocument.text'>"
                       "<office:body><office:text><table:table>";
        for (int i = 0; i < 50000; ++i) x += "<table:table-row-group>";
        x += "<table:table-row><table:table-cell><text:p>deep</text:p></table:table-cell></table:table-row>";
        for (int i = 0; i < 50000; ++i) x += "</table:table-row-group>";
        x += "</table:table></office:text></office:body></office:document>";
        QTemporaryDir dir;
        QFile f(dir.filePath("deep.fodt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(x);
        f.close();
        const LoadResult r = loadDocument(f.fileName());  // must not crash
        Q_UNUSED(r);
        bool ok = true;
        QCOMPARE(odf::toPoints(u"1e999cm", &ok), odf::toPoints(u"0cm"));
        QVERIFY(qAbs(odf::toPoints(u"1e30in")) <= Limits::kMaxLength * 72);
    }

    void secExportIsAtomicAndNeverClobbersSource()
    {
        QTemporaryDir dir;
        flow::Document model;
        Paragraph p;
        Run r;
        r.text = QStringLiteral("source");
        p.runs << r;
        model.body << Block::paragraph(p);
        FlowDocument flowDoc(DocumentKind::Rtf, model, QString());
        const QString pdf = dir.filePath("a.pdf");
        QString err;
        QVERIFY(Exporter::exportPdf(flowDoc, pdf, 0, &err));

        // Exporting an open PDF onto itself (rotated) must not corrupt it.
        LoadResult lr = loadDocument(pdf);
        QVERIFY(lr.document);
        QVERIFY(Exporter::exportPdf(*lr.document, pdf, 90, &err));
        QCOMPARE(lr.document->pageText(0).trimmed(), QStringLiteral("source"));
        const LoadResult again = loadDocument(pdf);
        QVERIFY(again.document);
        QCOMPARE(again.document->pageSize(0).width(), lr.document->pageSize(0).height());

        // A failing export leaves nothing behind.
        const QString bad = dir.filePath("missing-dir/out.pdf");
        QVERIFY(!Exporter::exportPdf(flowDoc, bad, 0, &err));
        QVERIFY(!QFile::exists(bad));
    }

    void secRenderSizeIsBounded()
    {
        flow::Document model;
        model.page.size = QSizeF(1e300, std::numeric_limits<double>::quiet_NaN());
        Paragraph p;
        Run r;
        r.text = QStringLiteral("x");
        r.style.size = 1e12;
        r.style.letterSpacing = std::numeric_limits<double>::infinity();
        p.runs << r;
        p.style.leftIndent = -1e200;
        model.body << Block::paragraph(p);
        FlowDocument doc(DocumentKind::Rtf, model, QString());
        QVERIFY(doc.pageSize(0).width() <= Limits::kMaxPageSide);
        // A huge bitmap request is refused rather than allocated.
        QVERIFY(doc.renderPage(0, 100.0, QRect(0, 0, 100000, 100000)).isNull());
        QVERIFY(!doc.renderPage(0, 1.0, QRect(0, 0, 200, 200)).isNull());
    }
};

QTEST_MAIN(TestImporters)
#include "tst_importers.moc"
