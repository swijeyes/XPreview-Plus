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
#include "pdf/PdfDocument.h"
#include "pdf/PdfSecurity.h"
#include "ps/PostScriptConverter.h"
#include "rtf/RtfImporter.h"
#include "rtf/RtfTokenizer.h"

#include <QPdfWriter>
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

    // A one-page AcroForm: text field "name" (100,650)-(400,680), check box
    // "agree" (100,600)-(120,620) and combo box "color", in PDF user space.
    static QByteArray formPdf()
    {
        QList<QByteArray> objs = {
            "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R 5 0 R 8 0 R] /NeedAppearances true"
            " /DR << /Font << /Helv 9 0 R >> >> /DA (/Helv 12 Tf 0 g) >> >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Annots [4 0 R 5 0 R 8 0 R]"
            " /Resources << /Font << /Helv 9 0 R >> >> >>",
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [100 650 400 680] /P 3 0 R /F 4"
            " /DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>",
            "<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /Rect [100 600 120 620] /P 3 0 R /F 4"
            " /V /Off /AS /Off /MK << /BC [0 0 0] /CA (4) >> /DA (/ZaDb 0 Tf 0 g)"
            " /AP << /N << /Yes 6 0 R /Off 7 0 R >> >> >>",
            "<< /BBox [0 0 20 20] /Length 32 >>\nstream\n0 g 4 4 12 12 re f 0 G 0 0 20 20 re S\nendstream",
            "<< /BBox [0 0 20 20] /Length 18 >>\nstream\n0 G 0 0 20 20 re S\nendstream",
            "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (color) /Rect [100 540 300 565] /P 3 0 R"
            " /F 4 /Opt [(Red) (Green) (Blue)] /V (Red) /DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        };
        QByteArray pdf = "%PDF-1.7\n";
        QList<int> offsets;
        for (int i = 0; i < objs.size(); ++i) {
            offsets << pdf.size();
            pdf += QByteArray::number(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
        }
        const int xref = pdf.size();
        pdf += "xref\n0 " + QByteArray::number(objs.size() + 1) + "\n0000000000 65535 f \n";
        for (int o : offsets) pdf += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
        pdf += "trailer\n<< /Size " + QByteArray::number(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
               QByteArray::number(xref) + "\n%%EOF\n";
        return pdf;
    }

    // A PDF whose pages say "Page 1" ... "Page n" (612 x 792, except page 2
    // which is landscape).
    static QString numberedPdf(const QString &path, int n)
    {
        QPdfWriter w(path);
        w.setResolution(72);
        auto size = [](int i) { return i == 1 ? QSizeF(792, 612) : QSizeF(612, 792); };
        w.setPageSize(QPageSize(size(0), QPageSize::Point, QString(), QPageSize::ExactMatch));
        w.setPageMargins({});
        QPainter p(&w);
        for (int i = 0; i < n; ++i) {
            if (i > 0) {
                w.setPageSize(QPageSize(size(i), QPageSize::Point, QString(), QPageSize::ExactMatch));
                w.newPage();
            }
            p.setFont(QFont(QStringLiteral("Arimo"), 24));
            p.drawText(QPointF(72, 100), QStringLiteral("Page %1").arg(i + 1));
        }
        p.end();
        return path;
    }

    static QStringList pageLabels(PdfDocument &doc)
    {
        QStringList out;
        for (int i = 0; i < doc.pageCount(); ++i) out << doc.pageText(i).trimmed();
        return out;
    }

    static std::shared_ptr<PdfDocument> openPdf(const QString &path)
    {
        PdfDocument::LoadError err;
        return PdfDocument::open(path, {}, &err);
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
        QVERIFY(mism.knownExtension);
        // A document disguised under a program's extension: no mismatch is
        // reported, but the extension is not a document one.
        const SniffResult hta = sniffFile(write("c.hta", "%PDF-1.4\n"));
        QCOMPARE(hta.format, SniffedFormat::Pdf);
        QVERIFY(!hta.knownExtension);
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

    // gzip around a zlib stream from qCompress (2-byte header, 4-byte
    // Adler-32 trailer); the gzip CRC is not checked by the reader.
    static QByteArray gzip(const QByteArray &data)
    {
        const QByteArray z = qCompress(data);
        QByteArray out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
        out += z.mid(4 + 2, z.size() - 4 - 2 - 4);
        out += QByteArray(8, '\0');
        return out;
    }

    void secSvgzDecompressionBounded()
    {
        const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>";
        QCOMPARE(SafeImage::sanitizeSvg(gzip(svg)), svg);
        // A few hundred KB that inflate past the SVG limit are refused.
        const QByteArray bomb = gzip(svg + QByteArray(Limits::kMaxSvgBytes + 1, ' '));
        QVERIFY(bomb.size() < 1024 * 1024);
        QVERIFY(SafeImage::sanitizeSvg(bomb).isEmpty());
        QVERIFY(SafeImage::sanitizeSvg(bomb.left(bomb.size() / 2)).isEmpty());  // truncated
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

    void pdfFormFill()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("form.pdf"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(formPdf());
        f.close();
        auto doc = openPdf(path);
        QVERIFY(doc);
        QVERIFY(doc->hasForm());
        // Page points, origin top-left: the name field's centre.
        const QPointF name(250, 792 - 665), agree(110, 792 - 610);
        QCOMPARE(doc->formFieldAt(0, name), PdfDocument::kTextField);
        QCOMPARE(doc->formFieldAt(0, QPointF(500, 100)), -1);
        QVERIFY(!doc->isModified());

        doc->formMouse(0, name, PdfDocument::Mouse::Press, {});
        doc->formMouse(0, name, PdfDocument::Mouse::Release, {});
        QVERIFY(doc->formHasFocus());
        QVERIFY(doc->formText(QStringLiteral("Hello")));
        doc->formKey(Qt::Key_Backspace, {});
        doc->formText(QStringLiteral("o, wörld"));
        doc->formKey(Qt::Key_Left, {});
        doc->formText(QStringLiteral("!"));
        doc->formSelectAll();
        QCOMPARE(doc->formSelectedText(), QStringLiteral("Hello, wörl!d"));
        // Tab moves on to the check box; Space ticks it.
        doc->formKey(Qt::Key_Tab, {});
        QVERIFY(doc->formHasFocus());
        doc->formText(QStringLiteral(" "));
        doc->formCommit();
        QVERIFY(!doc->formHasFocus());
        QVERIFY(doc->isModified());
        QMap<QString, QString> values = doc->formValues();
        QCOMPARE(values.value(QStringLiteral("name")), QStringLiteral("Hello, wörl!d"));
        QCOMPARE(values.value(QStringLiteral("agree")), QStringLiteral("On"));
        QCOMPARE(values.value(QStringLiteral("color")), QStringLiteral("Red"));
        // The typed text is drawn: dark pixels inside the name field.
        const QImage page = doc->renderPage(0, 2.0, QRect(0, 0, 1224, 1584));
        if (qEnvironmentVariableIsSet("XPREVIEW_DUMP")) page.save(qEnvironmentVariable("XPREVIEW_DUMP"));
        int dark = 0;
        for (int y = 2 * (792 - 678); y < 2 * (792 - 652); ++y)
            for (int x = 2 * 104; x < 2 * 396; ++x) dark += qGray(page.pixel(x, y)) < 100;
        QVERIFY2(dark > 100, qPrintable(QString::number(dark)));

        // Saved values survive a reload.
        const QString out = dir.filePath(QStringLiteral("filled.pdf"));
        QString err;
        QVERIFY2(doc->save(out, &err), qPrintable(err));
        QVERIFY(!doc->isModified());
        QCOMPARE(doc->filePath(), out);
        auto again = openPdf(out);
        QVERIFY(again);
        values = again->formValues();
        QCOMPARE(values.value(QStringLiteral("name")), QStringLiteral("Hello, wörl!d"));
        QCOMPARE(values.value(QStringLiteral("agree")), QStringLiteral("On"));
        // The original file is untouched.
        QCOMPARE(openPdf(path)->formValues().value(QStringLiteral("name")), QString());
    }

    void pdfPageEditing()
    {
        QTemporaryDir dir;
        auto doc = openPdf(numberedPdf(dir.filePath(QStringLiteral("a.pdf")), 4));
        QVERIFY(doc);
        QCOMPARE(pageLabels(*doc), QStringList({"Page 1", "Page 2", "Page 3", "Page 4"}));
        const int rev = doc->structureRevision();

        // Move the last page to the front, then pages 1 and 3 to the end.
        QCOMPARE(doc->movePages({3}, 0), 0);
        QCOMPARE(pageLabels(*doc), QStringList({"Page 4", "Page 1", "Page 2", "Page 3"}));
        QCOMPARE(doc->movePages({1, 3}, 4), 2);
        QCOMPARE(pageLabels(*doc), QStringList({"Page 4", "Page 2", "Page 1", "Page 3"}));
        QCOMPARE(doc->movePages({1}, 1), -1);  // no-op
        QVERIFY(doc->structureRevision() > rev);
        QVERIFY(doc->isModified());
        QCOMPARE(doc->pageSize(1), QSizeF(792, 612));  // sizes follow their pages

        // Rotation swaps the page box.
        QVERIFY(doc->rotatePages({1}, 1));
        QCOMPARE(doc->pageSize(1), QSizeF(612, 792));

        // Delete, but never every page.
        QString err;
        QVERIFY(!doc->deletePages({0, 1, 2, 3}, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(doc->deletePages({0, 2}, &err));
        QCOMPARE(pageLabels(*doc), QStringList({"Page 2", "Page 3"}));

        // Combine: another PDF's pages, and copies of our own.
        QFile other(numberedPdf(dir.filePath(QStringLiteral("b.pdf")), 2));
        QVERIFY(other.open(QIODevice::ReadOnly));
        QCOMPARE(doc->insertPdf(other.readAll(), {}, 1, &err), 2);
        QCOMPARE(pageLabels(*doc), QStringList({"Page 2", "Page 1", "Page 2", "Page 3"}));
        const QByteArray copy = doc->extractPages({3}, &err);
        QVERIFY(!copy.isEmpty());
        QCOMPARE(doc->insertPdf(copy, {}, 0, &err), 1);
        QCOMPARE(pageLabels(*doc), QStringList({"Page 3", "Page 2", "Page 1", "Page 2", "Page 3"}));
        QVERIFY(doc->insertBlankPage(5, QSizeF(300, 400)));
        QCOMPARE(doc->pageCount(), 6);
        QCOMPARE(doc->pageSize(5), QSizeF(300, 400));
        QCOMPARE(doc->insertPdf("not a pdf", {}, 0, &err), 0);

        // Undo walks back step by step, redo forward again.
        QVERIFY(doc->undo());
        QCOMPARE(doc->pageCount(), 5);
        QVERIFY(doc->undo());
        QVERIFY(doc->undo());
        QCOMPARE(pageLabels(*doc), QStringList({"Page 2", "Page 3"}));
        QVERIFY(doc->redo());
        QCOMPARE(pageLabels(*doc), QStringList({"Page 2", "Page 1", "Page 2", "Page 3"}));
        while (doc->canUndo()) QVERIFY(doc->undo());
        QCOMPARE(pageLabels(*doc), QStringList({"Page 1", "Page 2", "Page 3", "Page 4"}));
        QVERIFY(!doc->isModified());  // back where it was loaded
        while (doc->canRedo()) QVERIFY(doc->redo());
        QCOMPARE(doc->pageCount(), 6);

        // Save in place: the file now has the edited pages.
        const QString path = doc->filePath();
        QVERIFY2(doc->save(path, &err), qPrintable(err));
        QVERIFY(!doc->isModified());
        auto reopened = openPdf(path);
        QVERIFY(reopened);
        QCOMPARE(reopened->pageCount(), 6);
        QCOMPARE(pageLabels(*reopened).mid(0, 5), QStringList({"Page 3", "Page 2", "Page 1", "Page 2", "Page 3"}));
        // Still editable after saving (now backed by memory).
        QVERIFY(doc->deletePages({5}, &err));
        QVERIFY(doc->isModified());
        // Export writes the edits, not the file on disk.
        const QString exported = dir.filePath(QStringLiteral("export.pdf"));
        QVERIFY(Exporter::exportPdf(*doc, exported, 0, &err));
        QCOMPARE(openPdf(exported)->pageCount(), 5);
    }

    void pdfEncryption()
    {
        if (!PdfSecurity::available()) QSKIP("built without qpdf");
        QTemporaryDir dir;
        auto doc = openPdf(numberedPdf(dir.filePath(QStringLiteral("plain.pdf")), 3));
        QVERIFY(doc && !doc->isEncrypted() && !doc->isRestricted());

        PdfDocument::Protection p;
        p.mode = PdfDocument::Protection::Mode::Custom;
        p.openPassword = "s3cret pässword";
        p.ownerPassword = "owner";
        p.allowPrint = false;
        p.allowChanges = false;
        QString err;
        PdfDocument::Protection noOwner = p;
        noOwner.ownerPassword.clear();
        QVERIFY(!doc->setProtection(noOwner, &err));  // restrictions need an owner password
        QVERIFY2(doc->setProtection(p, &err), qPrintable(err));
        QVERIFY(doc->isModified());
        const QString locked = dir.filePath(QStringLiteral("locked.pdf"));
        QVERIFY2(doc->save(locked, &err), qPrintable(err));
        QVERIFY(QFileInfo(locked).size() > 0);

        // No password, or a wrong one: refused.
        PdfDocument::LoadError e;
        QVERIFY(!PdfDocument::open(locked, {}, &e));
        QCOMPARE(e, PdfDocument::LoadError::Password);
        QVERIFY(!PdfDocument::open(locked, "wrong", &e));
        // The open password: readable, AES-256, restricted.
        auto user = PdfDocument::open(locked, p.openPassword, &e);
        QVERIFY(user);
        QVERIFY(user->isEncrypted());
        QCOMPARE(user->encryptionDescription(), QStringLiteral("AES, 256-bit"));
        QVERIFY(!user->canPrint() && user->canCopy() && !user->canEditPages());
        QCOMPARE(pageLabels(*user), QStringList({"Page 1", "Page 2", "Page 3"}));
        QVERIFY(!user->deletePages({0}, &err));
        QCOMPARE(user->movePages({2}, 0), -1);
        QVERIFY(!user->setProtection({}, &err));  // only the owner may
        QVERIFY(user->extractPages({0}, &err).isEmpty());
        // The owner password lifts everything.
        auto owner = PdfDocument::open(locked, "owner", &e);
        QVERIFY(owner && !owner->isRestricted());

        // Editing keeps the original protection on save.
        QVERIFY(owner->deletePages({1}, &err));
        const QString edited = dir.filePath(QStringLiteral("edited.pdf"));
        QVERIFY2(owner->save(edited, &err), qPrintable(err));
        QVERIFY(!PdfDocument::open(edited, {}, &e));
        auto again = PdfDocument::open(edited, p.openPassword, &e);
        QVERIFY(again && !again->canPrint());
        QCOMPARE(pageLabels(*again), QStringList({"Page 1", "Page 3"}));
        // Undo still works on an encrypted document (snapshots are internal).
        QVERIFY(owner->undo());
        QCOMPARE(owner->pageCount(), 3);

        // Removing the protection writes a normal PDF.
        QVERIFY(owner->setProtection({}, &err));
        const QString open = dir.filePath(QStringLiteral("open.pdf"));
        QVERIFY(owner->save(open, &err));
        auto plain = PdfDocument::open(open, {}, &e);
        QVERIFY(plain && !plain->isEncrypted());
        // Export of an encrypted, edited document stays encrypted.
        QVERIFY(again->rotatePages({0}, 1) == false);  // restricted
        auto owner2 = PdfDocument::open(edited, "owner", &e);
        QVERIFY(owner2->rotatePages({0}, 1));
        const QString exported = dir.filePath(QStringLiteral("export.pdf"));
        QVERIFY(Exporter::exportPdf(*owner2, exported, 0, &err));
        QVERIFY(!PdfDocument::open(exported, {}, &e));
        QCOMPARE(e, PdfDocument::LoadError::Password);
    }

    void postScriptAndEps()
    {
        if (PostScript::ghostscriptPath().isEmpty()) QSKIP("Ghostscript not installed");
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly)) return QString();
            f.write(data);
            return f.fileName();
        };
        const QString eps = write(QStringLiteral("logo.eps"),
                                  "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200 100\n"
                                  "/Helvetica findfont 24 scalefont setfont 10 40 moveto (Hello EPS) show\n"
                                  "0 0 1 setrgbcolor 150 10 40 80 rectfill showpage\n");
        LoadResult r = loadDocument(eps);
        QVERIFY2(r.document, qPrintable(r.error));
        QCOMPARE(r.sniff.format, SniffedFormat::Eps);
        QCOMPARE(r.document->kindName(), QStringLiteral("EPS image"));
        QCOMPARE(r.document->pageCount(), 1);
        QCOMPARE(r.document->pageSize(0), QSizeF(200, 100));  // cropped to the bounding box
        QVERIFY(r.document->pageText(0).contains(QStringLiteral("Hello EPS")));
        // Saving never overwrites the source; Save As a PDF works.
        auto *pdf = static_cast<PdfDocument *>(r.document.get());
        QString err;
        QVERIFY(!pdf->save(eps, &err));
        QVERIFY(pdf->save(dir.filePath(QStringLiteral("logo.pdf")), &err));

        const QString ps = write(QStringLiteral("two.ps"),
                                 "%!PS-Adobe-3.0\n<< /PageSize [300 400] >> setpagedevice\n"
                                 "/Helvetica findfont 20 scalefont setfont\n"
                                 "72 300 moveto (First) show showpage 72 300 moveto (Second) show showpage\n");
        r = loadDocument(ps);
        QVERIFY2(r.document, qPrintable(r.error));
        QCOMPARE(r.sniff.format, SniffedFormat::PostScript);
        QCOMPARE(r.document->pageCount(), 2);
        QVERIFY(r.document->pageText(1).contains(QStringLiteral("Second")));
    }

    void postScriptIsSandboxed()
    {
        if (PostScript::ghostscriptPath().isEmpty()) QSKIP("Ghostscript not installed");
        QTemporaryDir dir;
        // A program that tries to read a file outside its input and to
        // write next to the user's files, and prints what it read.
        const QString target = dir.filePath(QStringLiteral("dropped.txt"));
        QFile f(dir.filePath(QStringLiteral("evil.ps")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("%!PS\n/Helvetica findfont 12 scalefont setfont 72 700 moveto\n"
                "(/etc/passwd) (r) file 200 string readstring pop show\n"
                "(" + target.toUtf8() + ") (w) file (pwned) writestring\nshowpage\n");
        f.close();
        LoadResult r = loadDocument(f.fileName());
        if (r.document) QVERIFY(!r.document->pageText(0).contains(QStringLiteral("root")));
        QVERIFY(!QFile::exists(target));
        // Same with -dNOSAFER smuggled into the environment.
        qputenv("GS_OPTIONS", "-dNOSAFER");
        r = loadDocument(f.fileName());
        qunsetenv("GS_OPTIONS");
        if (r.document) QVERIFY(!r.document->pageText(0).contains(QStringLiteral("root")));
        QVERIFY(!QFile::exists(target));
    }
};

QTEST_MAIN(TestImporters)
#include "tst_importers.moc"
