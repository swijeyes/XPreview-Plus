// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "core/Document.h"

#include <QHash>

#include <atomic>
#include <functional>

struct fpdf_document_t__;
struct fpdf_page_t__;
struct fpdf_textpage_t__;
struct fpdf_form_handle_t__;

// PDFium-backed PDF. PDFium is not thread-safe, so every call into it goes
// through one process-wide lock (pdfiumLock()).
//
// Besides viewing, a PDF can be edited: interactive form fields are filled
// in through PDFium's form-fill environment, and pages can be reordered,
// rotated, deleted and inserted (from other PDFs). Edits live in memory
// until save() writes them out. Page edits can be undone; each undo step is
// a snapshot of the whole document.
class PdfDocument : public Document {
public:
    enum class LoadError { None, File, Format, Password, Security, Unknown };
    // What a change notification is about (see setChangeHandler()).
    enum class Change { Appearance, Modified, FormFocus };
    enum class Mouse { Move, Press, Release, DoubleClick };

    static std::shared_ptr<PdfDocument> open(const QString &path, const QByteArray &password,
                                             LoadError *error);
    // PDF bytes converted from another format (PostScript, EPS). The
    // document shows `path` but is never written back to it.
    static std::shared_ptr<PdfDocument> openConverted(const QByteArray &pdf, const QString &path,
                                                      const QString &kindName, LoadError *error);
    ~PdfDocument() override;

    DocumentKind kind() const override { return DocumentKind::Pdf; }
    QString kindName() const override;
    bool isConverted() const { return !m_convertedKind.isEmpty(); }
    int pageCount() const override { return m_pageSizes.size(); }
    QSizeF pageSize(int page) const override;
    QImage renderPage(int page, double scale, const QRect &pixelRect) override;
    void paintPage(int page, QPainter *painter) override;

    bool hasText() const override { return true; }
    QString pageText(int page) override;
    QVector<TextHit> search(const QString &query, bool caseSensitive) override;
    TextHit selectText(int page, QPointF from, QPointF to) override;
    QString linkAt(int page, QPointF pt) override;

    QList<OutlineItem> outline() override;
    QMap<QString, QString> metadata() override;

    QString nativePdfPath() const override { return filePath(); }
    bool savePdfCopy(const QString &path, int extraRotation, QString *error) override;

    static void initLibrary();

    // Called on the GUI thread (queued by the caller if needed) when field
    // appearances change (page index, or -1 for all pages), when the
    // modified state changes, or when a form field gains or loses focus.
    // May be invoked with PDFium's lock held: handlers must not call back
    // into the document synchronously.
    void setChangeHandler(std::function<void(Change, int page)> handler);

    // --- Security -----------------------------------------------------------
    // Password protection written when the document is saved or exported.
    struct Protection {
        enum class Mode {
            None,      // not encrypted
            Original,  // as the file was: same passwords and permissions
            Custom,    // AES-256 with the passwords and permissions below
        };
        Mode mode = Mode::None;
        QByteArray openPassword;   // empty: anyone can open the file
        QByteArray ownerPassword;  // lifts the restrictions
        bool allowPrint = true, allowCopy = true, allowChanges = true;
        bool restricts() const { return !(allowPrint && allowCopy && allowChanges); }
    };
    bool isEncrypted() const { return m_securityRevision >= 0; }  // the file as opened
    QString encryptionDescription() const;
    // What whoever opened the document may do. The owner password, or an
    // unencrypted file, allows everything.
    bool canPrint() const { return m_permissions & 0x4; }
    bool canCopy() const { return m_permissions & 0x10; }
    bool canEditPages() const { return m_permissions & (0x8 | 0x400); }
    bool canFillForms() const { return m_permissions & (0x20 | 0x100); }
    bool isRestricted() const { return !(canPrint() && canCopy() && canEditPages() && canFillForms()); }
    const Protection &protection() const { return m_protection; }
    // Takes effect when the document is next saved; marks it modified.
    bool setProtection(const Protection &protection, QString *error);

    // --- Form filling -----------------------------------------------------
    static constexpr int kTextField = 6;  // FPDF_FORMFIELD_TEXTFIELD
    // Fillable fields the opener is allowed to fill.
    bool hasForm() const { return m_hasForm && canFillForms(); }
    // Field type (FPDF_FORMFIELD_*) under a point in page points, or -1.
    int formFieldAt(int page, QPointF pt);
    // Mouse and keyboard input for form fields. Return true if a field used it.
    bool formMouse(int page, QPointF pt, Mouse event, Qt::KeyboardModifiers mods);
    static bool isFormKey(int qtKey);  // a non-character key formKey() handles
    bool formKey(int qtKey, Qt::KeyboardModifiers mods);
    bool formText(const QString &text);
    bool formHasFocus() const { return m_formFocusPage >= 0; }
    int formFocusPage() const { return m_formFocusPage; }
    // Commits the field being edited (if any) and removes the focus.
    void formCommit();
    QString formSelectedText();
    void formReplaceSelection(const QString &text);
    void formSelectAll();
    bool formUndo();
    bool formRedo();
    // Field name -> value (text fields, choices, "On"/"Off" for buttons).
    QMap<QString, QString> formValues();

    // --- Page editing -----------------------------------------------------
    bool isModified() const { return m_modified; }
    // Increments with every change to the page structure.
    int structureRevision() const { return m_revision; }
    // Moves `pages` so they sit, in order, before page `before` (indices
    // before the move; pageCount() = after the last page). Returns the new
    // index of the first moved page, or -1 if nothing changed.
    int movePages(QVector<int> pages, int before);
    bool deletePages(QVector<int> pages, QString *error);
    // Rotates each page by `quarterTurns` × 90° clockwise (saved in the file).
    bool rotatePages(const QVector<int> &pages, int quarterTurns);
    bool insertBlankPage(int index, QSizeF size);
    // Inserts every page of a PDF given as bytes. Returns the number of pages
    // inserted (0 on failure).
    int insertPdf(const QByteArray &pdf, const QByteArray &password, int index, QString *error);
    // A new PDF holding copies of `pages`.
    QByteArray extractPages(const QVector<int> &pages, QString *error);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString undoLabel() const { return m_undo.isEmpty() ? QString() : m_undo.last().label; }
    QString redoLabel() const { return m_redo.isEmpty() ? QString() : m_redo.last().label; }
    bool undo();
    bool redo();

    // The whole document, with edits, as PDF bytes.
    QByteArray saveToBytes(QString *error);
    // Writes the document to `path` (atomically), which becomes its file.
    bool save(const QString &path, QString *error);

private:
    struct FormInfo;
    struct Snapshot {
        QByteArray data;
        int state;  // m_state when taken
        QString label;
    };

    PdfDocument();
    // Loads (or reuses) a page. Callbacks from inside PDFium pass
    // mayEvict = false: closing another page there could free one PDFium is using.
    fpdf_page_t__ *page(int index, bool mayEvict = true);
    fpdf_textpage_t__ *textPage(int index);
    void closePage(int index);
    void releasePages();
    QRectF charBoxToPage(int page, double l, double t, double r, double b);
    QPointF pageToPdf(int page, QPointF pt);
    QVector<PageRect> rangeBoxes(int page, int start, int count);
    QImage render(int page, double scale, const QRect &pixelRect, int flags);
    void readPageSizes();
    void initForm();
    void exitForm();
    void updateFormFocus();
    void notify(Change change, int page);
    // The document's content changed (new state), or returned to `state`.
    void setState(int state);
    void contentChanged() { setState(++m_lastState); }
    // Page edits: snapshot() before, finishEdit() after a successful change
    // (both with the lock held).
    bool snapshot(const QString &label, Snapshot *out);
    void finishEdit(Snapshot &&before);
    // The document, decrypted, as PDFium writes it (lock held).
    bool writePlain(QIODevice *out);
    QByteArray plainBytes(QString *error);  // commits form edits first
    // Plain bytes with m_protection applied: what goes into files.
    QByteArray protect(const QByteArray &plain, QString *error) const;
    bool loadFromMemory(const QByteArray &data, bool newStructure);

    fpdf_document_t__ *m_doc = nullptr;
    QByteArray m_memory;  // backing store when loaded from memory
    QByteArray m_password;
    QString m_convertedKind;       // e.g. "EPS image"; empty for real PDFs
    int m_securityRevision = -1;   // of the file as opened; -1 = not encrypted
    unsigned long m_permissions = 0xffffffff;
    QByteArray m_encryptedOriginal;  // the encrypted file, for Protection::Mode::Original
    Protection m_protection;
    QVector<QSizeF> m_pageSizes;
    // Small LRU of loaded pages; PDFium pages are expensive to keep forever.
    QHash<int, fpdf_page_t__ *> m_pages;
    QHash<int, fpdf_textpage_t__ *> m_textPages;
    QList<int> m_pageOrder;

    std::unique_ptr<FormInfo> m_formInfo;
    fpdf_form_handle_t__ *m_form = nullptr;
    bool m_hasForm = false;
    std::atomic<int> m_formFocusPage{-1};
    std::function<void(Change, int)> m_onChange;

    // Every distinct content gets a state number; the document is modified
    // when its state differs from the one last saved (or loaded).
    int m_state = 0, m_savedState = 0, m_lastState = 0;
    std::atomic<bool> m_modified{false};
    std::atomic<int> m_revision{0};
    QList<Snapshot> m_undo, m_redo;
};
