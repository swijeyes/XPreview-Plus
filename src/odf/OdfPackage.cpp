// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "OdfPackage.h"

#include "core/Limits.h"

#include <QFile>

#include <miniz.h>

QString OdfPackage::normalise(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    return name;
}

static bool safeEntryName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char(':')))
        return false;
    for (const QString &part : name.split(QLatin1Char('/')))
        if (part == QLatin1String("..")) return false;
    return true;
}

bool OdfPackage::open(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    if (f.size() > qint64(Limits::kMaxTotalBytes)) {
        *error = QStringLiteral("File is too large");
        return false;
    }
    m_zip = f.readAll();

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, m_zip.constData(), size_t(m_zip.size()), 0)) {
        *error = QStringLiteral("Not a valid ZIP package");
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > mz_uint(Limits::kMaxZipEntries)) {
        mz_zip_reader_end(&zip);
        *error = QStringLiteral("Package has too many entries");
        return false;
    }
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        const QString name = normalise(QString::fromUtf8(st.m_filename));
        if (!safeEntryName(name)) continue;  // path traversal: ignore entry
        m_entries.insert(name, Entry{int(i), st.m_comp_size, st.m_uncomp_size});
    }
    mz_zip_reader_end(&zip);

    m_mime = QString::fromLatin1(read(QStringLiteral("mimetype"))).trimmed();
    return true;
}

QByteArray OdfPackage::read(const QString &name, QString *error)
{
    auto it = m_entries.constFind(normalise(name));
    if (it == m_entries.cend()) {
        if (error) *error = QStringLiteral("Missing entry %1").arg(name);
        return {};
    }
    const Entry &e = *it;
    if (e.uncompressed > Limits::kMaxEntryBytes ||
        m_totalRead + e.uncompressed > Limits::kMaxTotalBytes ||
        (e.compressed > 0 && double(e.uncompressed) / double(e.compressed) > Limits::kMaxCompressionRatio &&
         e.uncompressed > 1024 * 1024)) {
        if (error) *error = QStringLiteral("Entry %1 exceeds safety limits").arg(name);
        return {};
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, m_zip.constData(), size_t(m_zip.size()), 0)) return {};
    QByteArray out;
    out.resize(qsizetype(e.uncompressed));
    // The declared size bounds the output buffer, so a lying header cannot
    // make miniz write past it.
    const bool ok = mz_zip_reader_extract_to_mem(&zip, mz_uint(e.index), out.data(), size_t(out.size()), 0);
    mz_zip_reader_end(&zip);
    if (!ok) {
        if (error) *error = QStringLiteral("Corrupt entry %1").arg(name);
        return {};
    }
    m_totalRead += e.uncompressed;
    return out;
}
