// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// Read-only ODF package (ZIP) with hostile-input limits: entry count,
// per-entry and total uncompressed size, compression ratio and path
// traversal checks. Never resolves references outside the package.
class OdfPackage {
public:
    bool open(const QString &path, QString *error);
    QString mimeType() const { return m_mime; }
    bool contains(const QString &name) const { return m_entries.contains(normalise(name)); }
    QByteArray read(const QString &name, QString *error = nullptr);
    QStringList entries() const { return m_entries.keys(); }

    static QString normalise(QString name);

private:
    struct Entry {
        int index;
        quint64 compressed;
        quint64 uncompressed;
    };
    QByteArray m_zip;
    QHash<QString, Entry> m_entries;
    QString m_mime;
    quint64 m_totalRead = 0;
};
