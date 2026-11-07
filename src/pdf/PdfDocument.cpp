// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "PdfDocument.h"

#include "core/Limits.h"
#include "core/SafeImage.h"
#include "PdfSecurity.h"

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

std::recursive_mutex &pdfiumLock()
{
    static std::recursive_mutex m;
    return m;
}

constexpr int kMaxCachedPages = 24;
constexpr int kMaxUndo = 20;
// Undo snapshots are whole documents: cap their total size too.
constexpr qsizetype kMaxUndoBytes = 768ll * 1024 * 1024;

QString fromUtf16Buffer(const QVector<unsigned short> &buf)
{
    // Buffers are NUL-terminated UTF-16LE.
    int len = buf.size();
    while (len > 0 && buf[len - 1] == 0) --len;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(buf.constData()), len);
}

// UTF-16, NUL-terminated, as PDFium's FPDF_WIDESTRING parameters expect.
QVector<unsigned short> toUtf16Buffer(const QString &s)
{
    QVector<unsigned short> buf(s.size() + 1, 0);
    memcpy(buf.data(), s.utf16(), s.size() * sizeof(unsigned short));
    return buf;
}

QString metaText(FPDF_DOCUMENT doc, const char *tag)
{
    const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= 2) return {};
    QVector<unsigned short> buf(bytes / 2 + 1);
    FPDF_GetMetaText(doc, tag, buf.data(), bytes);
    return fromUtf16Buffer(buf);
}

struct FileWriter : FPDF_FILEWRITE {
    QIODevice *file;
};

int writeBlock(FPDF_FILEWRITE *self, const void *data, unsigned long size)
{
    auto *w = static_cast<FileWriter *>(self);
    return w->file->write(static_cast<const char *>(data), qint64(size)) == qint64(size) ? 1 : 0;
}

int fwlModifiers(Qt::KeyboardModifiers mods)
{
    int m = 0;
    if (mods & Qt::ShiftModifier) m |= FWL_EVENTFLAG_ShiftKey;
    if (mods & Qt::AltModifier) m |= FWL_EVENTFLAG_AltKey;
#ifdef Q_OS_MACOS
    // Qt reports Command as Control; PDFium's shortcut key on Apple is Meta.
    if (mods & Qt::ControlModifier) m |= FWL_EVENTFLAG_MetaKey;
    if (mods & Qt::MetaModifier) m |= FWL_EVENTFLAG_ControlKey;
#else
    if (mods & Qt::ControlModifier) m |= FWL_EVENTFLAG_ControlKey;
    if (mods & Qt::MetaModifier) m |= FWL_EVENTFLAG_MetaKey;
#endif
    return m;
}

int fwlKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_Backspace: return FWL_VKEY_Back;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return FWL_VKEY_Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter: return FWL_VKEY_Return;
    case Qt::Key_Escape: return FWL_VKEY_Escape;
    case Qt::Key_Space: return FWL_VKEY_Space;
    case Qt::Key_PageUp: return FWL_VKEY_Prior;
    case Qt::Key_PageDown: return FWL_VKEY_Next;
    case Qt::Key_End: return FWL_VKEY_End;
    case Qt::Key_Home: return FWL_VKEY_Home;
    case Qt::Key_Left: return FWL_VKEY_Left;
    case Qt::Key_Up: return FWL_VKEY_Up;
    case Qt::Key_Right: return FWL_VKEY_Right;
    case Qt::Key_Down: return FWL_VKEY_Down;
    case Qt::Key_Delete: return FWL_VKEY_Delete;
    default: return -1;
    }
}

// Form-fill timers (caret blink). PDFium asks for them from inside calls
// made under its lock; they run on the GUI thread and call back under it.
QHash<int, QTimer *> &formTimers()
{
    static QHash<int, QTimer *> timers;
    return timers;
}

int setFormTimer(FPDF_FORMFILLINFO *, int elapse, TimerCallback callback)
{
    static std::atomic<int> nextId{1};
    if (!qApp || !callback) return 0;
    const int id = nextId++;
    QMetaObject::invokeMethod(qApp, [id, elapse, callback] {
        auto *t = new QTimer(qApp);
        t->setInterval(qMax(10, elapse));
        QObject::connect(t, &QTimer::timeout, qApp, [id, callback] {
            if (!formTimers().contains(id)) return;
            std::lock_guard lock(pdfiumLock());
            callback(id);
        });
        formTimers().insert(id, t);
        t->start();
    });
    return id;
}

void killFormTimer(FPDF_FORMFILLINFO *, int id)
{
    if (!qApp) return;
    QMetaObject::invokeMethod(qApp, [id] {
        if (QTimer *t = formTimers().take(id)) {
            t->stop();
            t->deleteLater();
        }
    });
}

FPDF_SYSTEMTIME localTime(FPDF_FORMFILLINFO *)
{
    const QDateTime now = QDateTime::currentDateTime();
    FPDF_SYSTEMTIME t{};
    t.wYear = now.date().year();
    t.wMonth = now.date().month();
    t.wDayOfWeek = now.date().dayOfWeek() % 7;
    t.wDay = now.date().day();
    t.wHour = now.time().hour();
    t.wMinute = now.time().minute();
    t.wSecond = now.time().second();
    t.wMilliseconds = now.time().msec();
    return t;
}

}  // namespace

// The form-fill callbacks PDFium needs. Scripts, URLs and named actions in
// forms are deliberately not carried out.
struct PdfDocument::FormInfo : FPDF_FORMFILLINFO {
    PdfDocument *self = nullptr;

    explicit FormInfo(PdfDocument *doc)
    {
        FPDF_FORMFILLINFO &base = *this;
        base = FPDF_FORMFILLINFO{};
        self = doc;
        version = 1;
        FFI_Invalidate = [](FPDF_FORMFILLINFO *info, FPDF_PAGE page, double, double, double, double) {
            PdfDocument *d = static_cast<FormInfo *>(info)->self;
            for (auto it = d->m_pages.cbegin(); it != d->m_pages.cend(); ++it)
                if (it.value() == page) return d->notify(Change::Appearance, it.key());
            d->notify(Change::Appearance, -1);
        };
        FFI_SetCursor = [](FPDF_FORMFILLINFO *, int) {};  // the view sets cursors itself
        FFI_SetTimer = setFormTimer;
        FFI_KillTimer = killFormTimer;
        FFI_GetLocalTime = localTime;
        FFI_OnChange = [](FPDF_FORMFILLINFO *info) { static_cast<FormInfo *>(info)->self->contentChanged(); };
        FFI_GetPage = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT, int index) -> FPDF_PAGE {
            return static_cast<FormInfo *>(info)->self->page(index, false);
        };
        FFI_GetCurrentPage = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT) -> FPDF_PAGE {
            PdfDocument *d = static_cast<FormInfo *>(info)->self;
            return d->page(qMax(0, d->m_formFocusPage.load()), false);
        };
        FFI_GetRotation = [](FPDF_FORMFILLINFO *, FPDF_PAGE) { return 0; };
        FFI_ExecuteNamedAction = [](FPDF_FORMFILLINFO *, FPDF_BYTESTRING) {};
        FFI_SetTextFieldFocus = [](FPDF_FORMFILLINFO *, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) {};
        FFI_DoURIAction = [](FPDF_FORMFILLINFO *, FPDF_BYTESTRING) {};
        FFI_DoGoToAction = [](FPDF_FORMFILLINFO *, int, int, float *, int) {};
    }
};

void PdfDocument::initLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        FPDF_InitLibraryWithConfig(&config);
    });
}

PdfDocument::PdfDocument() = default;

std::shared_ptr<PdfDocument> PdfDocument::open(const QString &path, const QByteArray &password,
                                               LoadError *error)
{
    initLibrary();
    std::lock_guard lock(pdfiumLock());
    const QByteArray utf8 = QFile::encodeName(path);
    FPDF_DOCUMENT doc =
        FPDF_LoadDocument(utf8.constData(), password.isEmpty() ? nullptr : password.constData());
    if (!doc) {
        switch (FPDF_GetLastError()) {
        case FPDF_ERR_FILE: *error = LoadError::File; break;
        case FPDF_ERR_FORMAT: *error = LoadError::Format; break;
        case FPDF_ERR_PASSWORD: *error = LoadError::Password; break;
        case FPDF_ERR_SECURITY: *error = LoadError::Security; break;
        default: *error = LoadError::Unknown; break;
        }
        return nullptr;
    }
    std::shared_ptr<PdfDocument> d(new PdfDocument);
    d->m_doc = doc;
    d->m_password = password;
    d->setFilePath(path);
    d->m_securityRevision = FPDF_GetSecurityHandlerRevision(doc);
    d->m_permissions = FPDF_GetDocPermissions(doc);
    if (d->isEncrypted()) {
        // Saving re-encrypts like the original, which qpdf reads from here.
        d->m_protection.mode = Protection::Mode::Original;
        QFile f(path);
        if (f.size() <= Limits::kMaxInsertPdfBytes && f.open(QIODevice::ReadOnly)) d->m_encryptedOriginal = f.readAll();
    }
    d->readPageSizes();
    d->initForm();
    *error = LoadError::None;
    return d;
}

std::shared_ptr<PdfDocument> PdfDocument::openConverted(const QByteArray &pdf, const QString &path,
                                                        const QString &kindName, LoadError *error)
{
    initLibrary();
    std::lock_guard lock(pdfiumLock());
    std::shared_ptr<PdfDocument> d(new PdfDocument);
    d->m_memory = pdf;  // PDFium reads from this buffer for the document's lifetime
    d->m_doc = FPDF_LoadMemDocument64(d->m_memory.constData(), size_t(d->m_memory.size()), nullptr);
    if (!d->m_doc) {
        *error = LoadError::Format;
        return nullptr;
    }
    d->m_convertedKind = kindName;
    d->setFilePath(path);
    d->readPageSizes();
    d->initForm();
    *error = LoadError::None;
    return d;
}

QString PdfDocument::kindName() const
{
    return isConverted() ? m_convertedKind : QStringLiteral("PDF document");
}

QString PdfDocument::encryptionDescription() const
{
    switch (m_securityRevision) {
    case -1: return {};
    case 2: return QStringLiteral("RC4, 40-bit");
    case 3: return QStringLiteral("RC4, 128-bit");
    case 4: return QStringLiteral("AES or RC4, 128-bit");
    default: return QStringLiteral("AES, 256-bit");
    }
}

bool PdfDocument::setProtection(const Protection &protection, QString *error)
{
    std::lock_guard lock(pdfiumLock());
    if (isRestricted()) {
        if (error) *error = QStringLiteral("Only the owner of this PDF (with its owner password) can change its protection.");
        return false;
    }
    if (protection.mode == Protection::Mode::Original && m_encryptedOriginal.isEmpty()) {
        if (error) *error = QStringLiteral("The original protection is not available.");
        return false;
    }
    if (protection.mode == Protection::Mode::Custom) {
        if (protection.openPassword.isEmpty() && !protection.restricts()) {
            if (error) *error = QStringLiteral("Choose a password, or restrictions, to protect the PDF with.");
            return false;
        }
        if (protection.restricts() && protection.ownerPassword.isEmpty()) {
            if (error) *error = QStringLiteral("Restrictions need an owner password to lift them.");
            return false;
        }
    }
    if (protection.mode != Protection::Mode::None && !PdfSecurity::available()) {
        if (error) *error = QStringLiteral("This build of XPreview+ cannot write encrypted PDFs.");
        return false;
    }
    m_protection = protection;
    contentChanged();
    return true;
}

QByteArray PdfDocument::protect(const QByteArray &plain, QString *error) const
{
    switch (m_protection.mode) {
    case Protection::Mode::None: return plain;
    case Protection::Mode::Original:
        return PdfSecurity::encryptLike(plain, m_encryptedOriginal, m_password, error);
    case Protection::Mode::Custom: {
        PdfSecurity::Permissions allowed;
        allowed.print = m_protection.allowPrint;
        allowed.copy = m_protection.allowCopy;
        allowed.changes = m_protection.allowChanges;
        // Without restrictions the owner password is the open password.
        const QByteArray owner = m_protection.restricts() || !m_protection.ownerPassword.isEmpty()
                                     ? m_protection.ownerPassword
                                     : m_protection.openPassword;
        return PdfSecurity::encrypt(plain, m_protection.openPassword, owner, allowed, error);
    }
    }
    return {};
}

PdfDocument::~PdfDocument()
{
    std::lock_guard lock(pdfiumLock());
    // Best effort: passwords do not linger in freed memory.
    for (QByteArray *secret : {&m_password, &m_protection.openPassword, &m_protection.ownerPassword}) secret->fill('\0');
    exitForm();
    releasePages();
    if (m_doc) FPDF_CloseDocument(m_doc);
}

void PdfDocument::readPageSizes()
{
    const int n = FPDF_GetPageCount(m_doc);
    QVector<QSizeF> sizes;
    sizes.reserve(n);
    for (int i = 0; i < n; ++i) {
        FS_SIZEF size{};
        const QSizeF letter(612, 792);
        if (FPDF_GetPageSizeByIndexF(m_doc, i, &size))
            sizes << SafeImage::sanitizePageSize(QSizeF(size.width, size.height), letter);
        else
            sizes << letter;
    }
    m_pageSizes = sizes;
}

void PdfDocument::initForm()
{
    m_hasForm = FPDF_GetFormType(m_doc) != FORMTYPE_NONE;
    if (!m_hasForm) return;
    m_formInfo = std::make_unique<FormInfo>(this);
    m_form = FPDFDOC_InitFormFillEnvironment(m_doc, m_formInfo.get());
    if (!m_form) {
        m_hasForm = false;
        m_formInfo.reset();
        return;
    }
    // Fillable fields get a light blue tint, as in other viewers (0xBBGGRR).
    FPDF_SetFormFieldHighlightColor(m_form, FPDF_FORMFIELD_UNKNOWN, 0xFFD9C8);
    FPDF_SetFormFieldHighlightAlpha(m_form, 110);
}

void PdfDocument::exitForm()
{
    // Pages must leave the form environment before it goes away.
    releasePages();
    if (m_form) FPDFDOC_ExitFormFillEnvironment(m_form);
    m_form = nullptr;
    m_formInfo.reset();
    m_hasForm = false;
    if (m_formFocusPage != -1) {
        m_formFocusPage = -1;
        notify(Change::FormFocus, -1);
    }
}

void PdfDocument::setChangeHandler(std::function<void(Change, int)> handler)
{
    std::lock_guard lock(pdfiumLock());
    m_onChange = std::move(handler);
}

void PdfDocument::notify(Change change, int page)
{
    if (m_onChange) m_onChange(change, page);
}

void PdfDocument::setState(int state)
{
    m_state = state;
    const bool modified = m_state != m_savedState;
    if (modified != m_modified) {
        m_modified = modified;
        notify(Change::Modified, -1);
    }
}

void PdfDocument::closePage(int index)
{
    if (auto *t = m_textPages.take(index)) FPDFText_ClosePage(t);
    if (auto *p = m_pages.take(index)) {
        if (m_form) FORM_OnBeforeClosePage(p, m_form);
        FPDF_ClosePage(p);
    }
    m_pageOrder.removeOne(index);
}

void PdfDocument::releasePages()
{
    for (int index : m_pages.keys()) closePage(index);
    m_textPages.clear();
    m_pageOrder.clear();
}

QSizeF PdfDocument::pageSize(int page) const
{
    return page >= 0 && page < m_pageSizes.size() ? m_pageSizes[page] : QSizeF();
}

FPDF_PAGE PdfDocument::page(int index, bool mayEvict)
{
    if (index < 0 || index >= m_pageSizes.size()) return nullptr;
    m_pageOrder.removeOne(index);
    m_pageOrder.append(index);
    if (auto *p = m_pages.value(index)) return p;
    // Evict the least recently used pages, but never the one holding the
    // focused form field (PDFium keeps editing state on it).
    for (int i = 0; mayEvict && m_pageOrder.size() > kMaxCachedPages && i < m_pageOrder.size();) {
        const int victim = m_pageOrder[i];
        if (victim == index || victim == m_formFocusPage) {
            ++i;
            continue;
        }
        closePage(victim);
    }
    FPDF_PAGE p = FPDF_LoadPage(m_doc, index);
    if (p) {
        m_pages.insert(index, p);
        if (m_form) FORM_OnAfterLoadPage(p, m_form);
    }
    return p;
}

FPDF_TEXTPAGE PdfDocument::textPage(int index)
{
    FPDF_PAGE p = page(index);
    if (!p) return nullptr;
    if (auto *t = m_textPages.value(index)) return t;
    FPDF_TEXTPAGE t = FPDFText_LoadPage(p);
    if (t) m_textPages.insert(index, t);
    return t;
}

QImage PdfDocument::render(int index, double scale, const QRect &pixelRect, int flags)
{
    if (!renderSizeAllowed(pixelRect)) return {};
    QImage img(pixelRect.size(), QImage::Format_ARGB32);
    img.fill(Qt::white);
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return img;
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRA,
                                          img.bits(), int(img.bytesPerLine()));
    if (!bmp) return img;
    // Render the full page "device" offset so only the tile lands in the
    // bitmap. PDFium applies the page's /Rotate itself. QImage ARGB32 is
    // BGRA in memory on little-endian hosts, which matches FPDFBitmap_BGRA.
    const QSizeF size = m_pageSizes[index];
    const int fullW = qRound(size.width() * scale);
    const int fullH = qRound(size.height() * scale);
    FPDF_RenderPageBitmap(bmp, p, -pixelRect.x(), -pixelRect.y(), fullW, fullH, 0, flags);
    // Interactive form fields (with their current values) go on top.
    if (m_form) FPDF_FFLDraw(m_form, bmp, p, -pixelRect.x(), -pixelRect.y(), fullW, fullH, 0, flags);
    FPDFBitmap_Destroy(bmp);
    return img;
}

QImage PdfDocument::renderPage(int index, double scale, const QRect &pixelRect)
{
    return render(index, scale, pixelRect, FPDF_ANNOT);
}

void PdfDocument::paintPage(int index, QPainter *painter)
{
    // PDFium cannot paint into a QPainter, so print through a high
    // resolution raster. PDF export uses savePdfCopy() and stays vector.
    const QSizeF size = pageSize(index);
    // 300 dpi, reduced for very large pages so the bitmap stays bounded.
    const double scale = qMin(300.0 / 72.0, std::sqrt(double(Limits::kMaxRenderPixels) /
                                                      qMax(1.0, size.width() * size.height())));
    const QRect px(0, 0, qCeil(size.width() * scale), qCeil(size.height() * scale));
    // Printing: no field highlight.
    const QImage img = render(index, scale, px, FPDF_ANNOT | FPDF_PRINTING);
    painter->drawImage(QRectF(QPointF(0, 0), size), img);
}

QPointF PdfDocument::pageToPdf(int index, QPointF pt)
{
    FPDF_PAGE p = page(index);
    const QSizeF size = m_pageSizes[index];
    double x = 0, y = 0;
    FPDF_DeviceToPage(p, 0, 0, qRound(size.width() * 100), qRound(size.height() * 100), 0,
                      qRound(pt.x() * 100), qRound(pt.y() * 100), &x, &y);
    return {x, y};
}

QRectF PdfDocument::charBoxToPage(int index, double l, double t, double r, double b)
{
    FPDF_PAGE p = page(index);
    const QSizeF size = m_pageSizes[index];
    // Map through the device transform at 100x to keep sub-point precision.
    const int w = qRound(size.width() * 100), h = qRound(size.height() * 100);
    int x1, y1, x2, y2;
    FPDF_PageToDevice(p, 0, 0, w, h, 0, l, t, &x1, &y1);
    FPDF_PageToDevice(p, 0, 0, w, h, 0, r, b, &x2, &y2);
    return QRectF(QPointF(x1 / 100.0, y1 / 100.0), QPointF(x2 / 100.0, y2 / 100.0)).normalized();
}

QVector<PageRect> PdfDocument::rangeBoxes(int index, int start, int count)
{
    QVector<PageRect> out;
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return out;
    const int n = FPDFText_CountRects(t, start, count);
    for (int i = 0; i < n; ++i) {
        double l, top, r, b;
        if (FPDFText_GetRect(t, i, &l, &top, &r, &b))
            out << PageRect{index, charBoxToPage(index, l, top, r, b)};
    }
    return out;
}

QString PdfDocument::pageText(int index)
{
    std::lock_guard lock(pdfiumLock());
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return {};
    const int n = FPDFText_CountChars(t);
    if (n <= 0) return {};
    QVector<unsigned short> buf(n + 1);
    FPDFText_GetText(t, 0, n, buf.data());
    return fromUtf16Buffer(buf);
}

QVector<TextHit> PdfDocument::search(const QString &query, bool caseSensitive)
{
    QVector<TextHit> hits;
    if (query.isEmpty()) return hits;
    std::lock_guard lock(pdfiumLock());
    QVector<unsigned short> q = toUtf16Buffer(query);
    for (int i = 0; i < pageCount() && hits.size() < 5000; ++i) {
        FPDF_TEXTPAGE t = textPage(i);
        if (!t) continue;
        FPDF_SCHHANDLE h = FPDFText_FindStart(t, q.data(), caseSensitive ? FPDF_MATCHCASE : 0, 0);
        while (h && FPDFText_FindNext(h)) {
            const int start = FPDFText_GetSchResultIndex(h);
            const int count = FPDFText_GetSchCount(h);
            TextHit hit;
            hit.boxes = rangeBoxes(i, start, count);
            hit.text = query;
            if (!hit.boxes.isEmpty()) hits << hit;
        }
        if (h) FPDFText_FindClose(h);
    }
    return hits;
}

TextHit PdfDocument::selectText(int index, QPointF from, QPointF to)
{
    TextHit hit;
    std::lock_guard lock(pdfiumLock());
    FPDF_TEXTPAGE t = textPage(index);
    if (!t) return hit;
    const QPointF a = pageToPdf(index, from), b = pageToPdf(index, to);
    const double tol = 6.0;
    int i1 = FPDFText_GetCharIndexAtPos(t, a.x(), a.y(), tol, tol);
    int i2 = FPDFText_GetCharIndexAtPos(t, b.x(), b.y(), tol, tol);
    if (i1 < 0 || i2 < 0) {
        // Fall back to a rectangular selection when either end is off-text.
        const QRectF r = QRectF(a, b).normalized();
        const int len = FPDFText_GetBoundedText(t, r.left(), r.bottom(), r.right(), r.top(),
                                                nullptr, 0);
        if (len > 0) {
            QVector<unsigned short> buf(len + 1);
            FPDFText_GetBoundedText(t, r.left(), r.bottom(), r.right(), r.top(), buf.data(), len);
            hit.text = fromUtf16Buffer(buf);
            hit.boxes << PageRect{index, QRectF(from, to).normalized()};
        }
        return hit;
    }
    if (i1 > i2) std::swap(i1, i2);
    const int count = i2 - i1 + 1;
    QVector<unsigned short> buf(count + 1);
    FPDFText_GetText(t, i1, count, buf.data());
    hit.text = fromUtf16Buffer(buf);
    hit.boxes = rangeBoxes(index, i1, count);
    return hit;
}

QString PdfDocument::linkAt(int index, QPointF pt)
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return {};
    const QPointF pdf = pageToPdf(index, pt);
    FPDF_LINK link = FPDFLink_GetLinkAtPoint(p, pdf.x(), pdf.y());
    if (!link) return {};
    FPDF_ACTION action = FPDFLink_GetAction(link);
    if (action && FPDFAction_GetType(action) == PDFACTION_URI) {
        // Bounded: the length comes from the file, and an int cast of a huge
        // value would size the buffer wrongly.
        const unsigned long len = FPDFAction_GetURIPath(m_doc, action, nullptr, 0);
        if (len == 0 || len > 64 * 1024) return {};
        QByteArray buf(qsizetype(len), 0);
        if (FPDFAction_GetURIPath(m_doc, action, buf.data(), len) != len) return {};
        return QString::fromUtf8(buf.constData());
    }
    FPDF_DEST dest = FPDFLink_GetDest(m_doc, link);
    if (!dest && action) dest = FPDFAction_GetDest(m_doc, action);
    if (dest) return QStringLiteral("#page=%1").arg(FPDFDest_GetDestPageIndex(m_doc, dest));
    return {};
}

static void readBookmarks(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, QList<OutlineItem> &out,
                          int depth, int &budget)
{
    if (depth > 32) return;
    for (FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, parent); bm && budget > 0;
         bm = FPDFBookmark_GetNextSibling(doc, bm)) {
        --budget;
        OutlineItem item;
        const unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
        QVector<unsigned short> buf(bytes / 2 + 1);
        FPDFBookmark_GetTitle(bm, buf.data(), bytes);
        item.title = fromUtf16Buffer(buf);
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
        if (!dest) {
            if (FPDF_ACTION a = FPDFBookmark_GetAction(bm)) dest = FPDFAction_GetDest(doc, a);
        }
        if (dest) item.page = FPDFDest_GetDestPageIndex(doc, dest);
        readBookmarks(doc, bm, item.children, depth + 1, budget);
        out << item;
    }
}

QList<OutlineItem> PdfDocument::outline()
{
    std::lock_guard lock(pdfiumLock());
    QList<OutlineItem> items;
    int budget = 20000;  // guard against cyclic outlines
    readBookmarks(m_doc, nullptr, items, 0, budget);
    return items;
}

QMap<QString, QString> PdfDocument::metadata()
{
    std::lock_guard lock(pdfiumLock());
    QMap<QString, QString> m;
    for (const char *tag : {"Title", "Author", "Subject", "Creator", "Producer", "CreationDate"}) {
        const QString v = metaText(m_doc, tag);
        if (!v.isEmpty()) m.insert(QString::fromLatin1(tag), v);
    }
    int version = 0;
    if (FPDF_GetFileVersion(m_doc, &version))
        m.insert(QStringLiteral("PDF version"), QStringLiteral("%1.%2").arg(version / 10).arg(version % 10));
    m.insert(QStringLiteral("Pages"), QString::number(pageCount()));
    if (m_hasForm) m.insert(QStringLiteral("Form"), QStringLiteral("Fillable form fields"));
    if (isEncrypted()) {
        QStringList limits;
        if (!canPrint()) limits << QStringLiteral("printing");
        if (!canCopy()) limits << QStringLiteral("copying");
        if (!canEditPages() || !canFillForms()) limits << QStringLiteral("changes");
        m.insert(QStringLiteral("Security"),
                 QStringLiteral("Encrypted (%1)").arg(encryptionDescription()) +
                     (limits.isEmpty() ? QString() : QStringLiteral("; restricts %1").arg(limits.join(QStringLiteral(", ")))));
    }
    if (isConverted()) m.insert(QStringLiteral("Converted from"), m_convertedKind);
    return m;
}

bool PdfDocument::writePlain(QIODevice *out)
{
    FileWriter w{};
    w.version = 1;
    w.WriteBlock = writeBlock;
    w.file = out;
    // Decrypted: protection is applied separately (protect()), and snapshots
    // for undo stay readable without the password.
    return FPDF_SaveAsCopy(m_doc, &w, FPDF_NO_INCREMENTAL | FPDF_REMOVE_SECURITY);
}

bool PdfDocument::savePdfCopy(const QString &path, int extraRotation, QString *error)
{
    extraRotation = ((extraRotation % 360) + 360) % 360;
    // Writes go to a temporary file that atomically replaces the target, so
    // a failure never leaves a partial PDF and the source file (which
    // PDFium may still be reading) is never truncated in place.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    if (extraRotation == 0 && !m_modified && m_memory.isEmpty()) {
        // Unchanged and still backed by its file: lossless byte copy.
        if (QFileInfo(path).canonicalFilePath() == QFileInfo(filePath()).canonicalFilePath()) {
            out.cancelWriting();
            return true;  // exporting a PDF onto itself: nothing to do
        }
        QFile in(filePath());
        bool ok = in.open(QIODevice::ReadOnly);
        while (ok && !in.atEnd()) {
            const QByteArray chunk = in.read(1 << 20);
            ok = !chunk.isEmpty() && out.write(chunk) == chunk.size();
        }
        if (ok && out.commit()) return true;
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    std::lock_guard lock(pdfiumLock());
    QVector<int> original;
    if (extraRotation) {
        for (int i = 0; i < pageCount(); ++i) {
            FPDF_PAGE p = page(i);
            const int r = FPDFPage_GetRotation(p);
            original << r;
            FPDFPage_SetRotation(p, (r + extraRotation / 90) % 4);
        }
    }
    QBuffer plain;
    plain.open(QIODevice::WriteOnly);
    bool ok = writePlain(&plain);
    for (int i = 0; i < original.size(); ++i) FPDFPage_SetRotation(page(i), original[i]);
    QString protectError;
    const QByteArray data = ok ? protect(plain.data(), &protectError) : QByteArray();
    ok = !data.isEmpty() && out.write(data) == data.size() && out.commit();
    if (!ok && error)
        *error = protectError.isEmpty() ? QStringLiteral("Could not write %1").arg(path) : protectError;
    return ok;
}

// --- Form filling ------------------------------------------------------------

void PdfDocument::updateFormFocus()
{
    int index = -1;
    FPDF_ANNOTATION annot = nullptr;
    if (m_form && FORM_GetFocusedAnnot(m_form, &index, &annot) && annot) FPDFPage_CloseAnnot(annot);
    else index = -1;
    if (index != m_formFocusPage) {
        m_formFocusPage = index;
        notify(Change::FormFocus, index);
    }
}

int PdfDocument::formFieldAt(int index, QPointF pt)
{
    if (!hasForm()) return -1;
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return -1;
    const QPointF pdf = pageToPdf(index, pt);
    return FPDFPage_HasFormFieldAtPoint(m_form, p, pdf.x(), pdf.y());
}

bool PdfDocument::formMouse(int index, QPointF pt, Mouse event, Qt::KeyboardModifiers mods)
{
    if (!hasForm()) return false;
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(index);
    if (!p) return false;
    const QPointF pdf = pageToPdf(index, pt);
    const int m = fwlModifiers(mods);
    bool used = false;
    switch (event) {
    case Mouse::Move: used = FORM_OnMouseMove(m_form, p, m, pdf.x(), pdf.y()); break;
    case Mouse::Press: used = FORM_OnLButtonDown(m_form, p, m, pdf.x(), pdf.y()); break;
    case Mouse::Release: used = FORM_OnLButtonUp(m_form, p, m, pdf.x(), pdf.y()); break;
    case Mouse::DoubleClick: used = FORM_OnLButtonDoubleClick(m_form, p, m, pdf.x(), pdf.y()); break;
    }
    if (event != Mouse::Move) updateFormFocus();
    return used;
}

bool PdfDocument::isFormKey(int qtKey) { return fwlKey(qtKey) >= 0; }

bool PdfDocument::formKey(int qtKey, Qt::KeyboardModifiers mods)
{
    const int vkey = fwlKey(qtKey);
    if (!hasForm() || vkey < 0) return false;
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    if (!p) return false;
    const int m = fwlModifiers(mods) | (qtKey == Qt::Key_Backtab ? FWL_EVENTFLAG_ShiftKey : 0);
    bool used = FORM_OnKeyDown(m_form, p, vkey, m);
    // Like a Windows WM_CHAR: these keys also arrive as characters, which is
    // where PDFium's text fields delete and insert line breaks.
    if (vkey == FWL_VKEY_Back || vkey == FWL_VKEY_Return)
        used = FORM_OnChar(m_form, p, vkey, m) || used;
    FORM_OnKeyUp(m_form, p, vkey, m);
    updateFormFocus();
    return used;
}

bool PdfDocument::formText(const QString &text)
{
    if (!hasForm() || text.isEmpty()) return false;
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    if (!p) return false;
    bool used = false;
    for (QChar c : text)
        // Skips control characters and the private-use codes macOS gives
        // function keys.
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f && !(c.unicode() >= 0xf700 && c.unicode() <= 0xf8ff))
            used = FORM_OnChar(m_form, p, c.unicode(), 0) || used;
    updateFormFocus();
    return used;
}

void PdfDocument::formCommit()
{
    std::lock_guard lock(pdfiumLock());
    if (!m_form) return;
    FORM_ForceToKillFocus(m_form);
    updateFormFocus();
}

QString PdfDocument::formSelectedText()
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    if (!m_form || !p) return {};
    const unsigned long bytes = FORM_GetSelectedText(m_form, p, nullptr, 0);
    if (bytes <= 2) return {};
    QVector<unsigned short> buf(bytes / 2 + 1);
    FORM_GetSelectedText(m_form, p, buf.data(), bytes);
    return fromUtf16Buffer(buf);
}

void PdfDocument::formReplaceSelection(const QString &text)
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    if (!m_form || !p) return;
    const QVector<unsigned short> buf = toUtf16Buffer(text);
    FORM_ReplaceSelection(m_form, p, buf.constData());
}

void PdfDocument::formSelectAll()
{
    std::lock_guard lock(pdfiumLock());
    if (FPDF_PAGE p = page(m_formFocusPage); m_form && p) FORM_SelectAllText(m_form, p);
}

bool PdfDocument::formUndo()
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    return m_form && p && FORM_CanUndo(m_form, p) && FORM_Undo(m_form, p);
}

bool PdfDocument::formRedo()
{
    std::lock_guard lock(pdfiumLock());
    FPDF_PAGE p = page(m_formFocusPage);
    return m_form && p && FORM_CanRedo(m_form, p) && FORM_Redo(m_form, p);
}

QMap<QString, QString> PdfDocument::formValues()
{
    QMap<QString, QString> values;
    std::lock_guard lock(pdfiumLock());
    if (!m_form) return values;
    auto text = [](auto get) {
        const unsigned long bytes = get(nullptr, 0);
        if (bytes <= 2) return QString();
        QVector<unsigned short> buf(bytes / 2 + 1);
        get(buf.data(), bytes);
        return fromUtf16Buffer(buf);
    };
    for (int i = 0; i < pageCount(); ++i) {
        FPDF_PAGE p = page(i);
        if (!p) continue;
        const int n = FPDFPage_GetAnnotCount(p);
        for (int k = 0; k < n; ++k) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(p, k);
            if (!a) continue;
            if (FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET) {
                const QString name = text([&](FPDF_WCHAR *b, unsigned long l) { return FPDFAnnot_GetFormFieldName(m_form, a, b, l); });
                const int type = FPDFAnnot_GetFormFieldType(m_form, a);
                if (type == FPDF_FORMFIELD_CHECKBOX || type == FPDF_FORMFIELD_RADIOBUTTON) {
                    if (FPDFAnnot_IsChecked(m_form, a) || !values.contains(name))
                        values.insert(name, FPDFAnnot_IsChecked(m_form, a) ? QStringLiteral("On") : QStringLiteral("Off"));
                } else {
                    values.insert(name, text([&](FPDF_WCHAR *b, unsigned long l) { return FPDFAnnot_GetFormFieldValue(m_form, a, b, l); }));
                }
            }
            FPDFPage_CloseAnnot(a);
        }
    }
    return values;
}

// --- Page editing ------------------------------------------------------------

bool PdfDocument::snapshot(const QString &label, Snapshot *out)
{
    if (m_form) {
        FORM_ForceToKillFocus(m_form);
        updateFormFocus();
    }
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    if (!writePlain(&buf)) return false;
    *out = Snapshot{buf.data(), m_state, label};
    // Pages are reloaded after every structural change.
    releasePages();
    return true;
}

void PdfDocument::finishEdit(Snapshot &&before)
{
    m_undo.append(std::move(before));
    qsizetype total = 0;
    for (const Snapshot &s : std::as_const(m_undo)) total += s.data.size();
    while (m_undo.size() > 1 && (m_undo.size() > kMaxUndo || total > kMaxUndoBytes)) {
        total -= m_undo.first().data.size();
        m_undo.removeFirst();
    }
    m_redo.clear();
    releasePages();
    readPageSizes();
    ++m_revision;
    contentChanged();
}

int PdfDocument::movePages(QVector<int> pages, int before)
{
    if (!canEditPages()) return -1;  // the owner restricted changes
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    const int n = pageCount();
    if (pages.isEmpty() || pages.first() < 0 || pages.last() >= n || before < 0 || before > n) return -1;
    const int dest = before - int(std::count_if(pages.cbegin(), pages.cend(), [before](int p) { return p < before; }));
    const bool contiguous = pages.last() - pages.first() + 1 == pages.size();
    if (contiguous && dest == pages.first()) return -1;  // already there
    std::lock_guard lock(pdfiumLock());
    Snapshot before_;
    if (!snapshot(pages.size() == 1 ? QStringLiteral("Move Page") : QStringLiteral("Move Pages"), &before_)) return -1;
    if (!FPDF_MovePages(m_doc, pages.constData(), pages.size(), dest)) return -1;
    finishEdit(std::move(before_));
    return dest;
}

bool PdfDocument::deletePages(QVector<int> pages, QString *error)
{
    if (!canEditPages()) {
        if (error) *error = QStringLiteral("The owner of this PDF does not allow changes.");
        return false;
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    if (pages.isEmpty() || pages.first() < 0 || pages.last() >= pageCount()) return false;
    if (pages.size() >= pageCount()) {
        if (error) *error = QStringLiteral("A PDF must keep at least one page.");
        return false;
    }
    std::lock_guard lock(pdfiumLock());
    Snapshot before;
    if (!snapshot(pages.size() == 1 ? QStringLiteral("Delete Page") : QStringLiteral("Delete Pages"), &before)) {
        if (error) *error = QStringLiteral("The document could not be changed.");
        return false;
    }
    for (auto it = pages.crbegin(); it != pages.crend(); ++it) FPDFPage_Delete(m_doc, *it);
    finishEdit(std::move(before));
    return true;
}

bool PdfDocument::rotatePages(const QVector<int> &pages, int quarterTurns)
{
    if (!canEditPages()) return false;  // the owner restricted changes
    quarterTurns = ((quarterTurns % 4) + 4) % 4;
    if (pages.isEmpty() || quarterTurns == 0) return false;
    std::lock_guard lock(pdfiumLock());
    Snapshot before;
    if (!snapshot(QStringLiteral("Rotate"), &before)) return false;
    for (int i : pages) {
        FPDF_PAGE p = FPDF_LoadPage(m_doc, i);
        if (!p) continue;
        FPDFPage_SetRotation(p, (FPDFPage_GetRotation(p) + quarterTurns) % 4);
        FPDF_ClosePage(p);
    }
    finishEdit(std::move(before));
    return true;
}

bool PdfDocument::insertBlankPage(int index, QSizeF size)
{
    if (!canEditPages()) return false;  // the owner restricted changes
    index = qBound(0, index, pageCount());
    std::lock_guard lock(pdfiumLock());
    Snapshot before;
    if (!snapshot(QStringLiteral("Insert Blank Page"), &before)) return false;
    FPDF_PAGE p = FPDFPage_New(m_doc, index, size.width(), size.height());
    if (!p) return false;
    FPDF_ClosePage(p);
    finishEdit(std::move(before));
    return true;
}

int PdfDocument::insertPdf(const QByteArray &pdf, const QByteArray &password, int index, QString *error)
{
    if (!canEditPages()) {
        if (error) *error = QStringLiteral("The owner of this PDF does not allow changes.");
        return 0;
    }
    if (pdf.size() > Limits::kMaxInsertPdfBytes) {
        if (error) *error = QStringLiteral("The file is too large to insert.");
        return 0;
    }
    index = qBound(0, index, pageCount());
    std::lock_guard lock(pdfiumLock());
    FPDF_DOCUMENT src = FPDF_LoadMemDocument64(pdf.constData(), size_t(pdf.size()),
                                               password.isEmpty() ? nullptr : password.constData());
    if (!src) {
        if (error)
            *error = FPDF_GetLastError() == FPDF_ERR_PASSWORD ? QStringLiteral("The PDF is password protected.")
                                                               : QStringLiteral("The file is not a readable PDF.");
        return 0;
    }
    const int n = FPDF_GetPageCount(src);
    Snapshot before;
    bool ok = n > 0 && snapshot(QStringLiteral("Insert Pages"), &before);
    ok = ok && FPDF_ImportPagesByIndex(m_doc, src, nullptr, 0, index);
    FPDF_CloseDocument(src);
    if (!ok) {
        if (error) *error = QStringLiteral("The pages could not be inserted.");
        readPageSizes();
        ++m_revision;
        return 0;
    }
    finishEdit(std::move(before));
    return n;
}

QByteArray PdfDocument::extractPages(const QVector<int> &pages, QString *error)
{
    // Copies of pages leave the document's protection behind.
    if (!canEditPages() || !canCopy()) {
        if (error) *error = QStringLiteral("The owner of this PDF does not allow copying its pages.");
        return {};
    }
    std::lock_guard lock(pdfiumLock());
    if (m_form) {
        FORM_ForceToKillFocus(m_form);
        updateFormFocus();
    }
    FPDF_DOCUMENT out = FPDF_CreateNewDocument();
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    bool ok = out && FPDF_ImportPagesByIndex(out, m_doc, pages.constData(), pages.size(), 0);
    if (ok) {
        FileWriter w{};
        w.version = 1;
        w.WriteBlock = writeBlock;
        w.file = &buf;
        ok = FPDF_SaveAsCopy(out, &w, FPDF_NO_INCREMENTAL);
    }
    if (out) FPDF_CloseDocument(out);
    if (!ok) {
        if (error) *error = QStringLiteral("The pages could not be copied.");
        return {};
    }
    return buf.data();
}

bool PdfDocument::loadFromMemory(const QByteArray &data, bool newStructure)
{
    // Load first: on failure the current document stays as it is.
    // Always decrypted bytes (writePlain()): no password.
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data.constData(), size_t(data.size()), nullptr);
    if (!doc) return false;
    exitForm();
    FPDF_CloseDocument(m_doc);
    m_doc = doc;
    m_memory = data;  // shares the buffer PDFium reads from
    readPageSizes();
    initForm();
    if (newStructure) ++m_revision;
    return true;
}

bool PdfDocument::undo()
{
    std::lock_guard lock(pdfiumLock());
    if (m_undo.isEmpty()) return false;
    Snapshot current;
    if (!snapshot(m_undo.last().label, &current)) return false;
    const Snapshot target = m_undo.last();
    if (!loadFromMemory(target.data, true)) return false;
    m_undo.removeLast();
    m_redo.append(std::move(current));
    setState(target.state);
    return true;
}

bool PdfDocument::redo()
{
    std::lock_guard lock(pdfiumLock());
    if (m_redo.isEmpty()) return false;
    Snapshot current;
    if (!snapshot(m_redo.last().label, &current)) return false;
    const Snapshot target = m_redo.last();
    if (!loadFromMemory(target.data, true)) return false;
    m_redo.removeLast();
    m_undo.append(std::move(current));
    setState(target.state);
    return true;
}

QByteArray PdfDocument::plainBytes(QString *error)
{
    if (m_form) {
        FORM_ForceToKillFocus(m_form);
        updateFormFocus();
    }
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    if (!writePlain(&buf)) {
        if (error) *error = QStringLiteral("The document could not be written.");
        return {};
    }
    return buf.data();
}

QByteArray PdfDocument::saveToBytes(QString *error)
{
    std::lock_guard lock(pdfiumLock());
    const QByteArray plain = plainBytes(error);
    return plain.isEmpty() ? plain : protect(plain, error);
}

bool PdfDocument::save(const QString &path, QString *error)
{
    std::lock_guard lock(pdfiumLock());
    // A converted document (PostScript, EPS) never overwrites its source.
    if (isConverted() && QFileInfo(path).canonicalFilePath() == QFileInfo(filePath()).canonicalFilePath()) {
        if (error) *error = QStringLiteral("Save the converted document as a new PDF file.");
        return false;
    }
    const QByteArray plain = plainBytes(error);
    if (plain.isEmpty()) return false;
    const QByteArray data = protect(plain, error);
    if (data.isEmpty()) return false;
    // Continue from the saved content in memory, so the file being replaced
    // is no longer held open (Windows cannot replace an open file).
    if (!loadFromMemory(plain, false)) {
        if (error) *error = QStringLiteral("The document could not be written.");
        return false;
    }
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    m_convertedKind.clear();  // it is a PDF file now
    setFilePath(path);
    m_savedState = m_state;
    setState(m_state);
    return true;
}
