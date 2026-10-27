// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

// Collected by every importer: what was simplified, substituted or shown as a
// placeholder. Drives the "Simplified view" info bar.
class FidelityReport {
public:
    // Count an occurrence of a simplification, e.g. "Charts shown as images".
    void count(const QString &what, int n = 1) { m_counters[what] += n; }
    void note(const QString &message) {
        if (!m_notes.contains(message)) m_notes << message;
    }
    void fontSubstituted(const QString &from, const QString &to) {
        m_fonts.insert(from, to);
    }
    void error(const QString &message) { m_errors << message; }

    bool isNotable() const {
        return !m_counters.isEmpty() || !m_notes.isEmpty() || !m_fonts.isEmpty() ||
               !m_errors.isEmpty();
    }
    bool hasErrors() const { return !m_errors.isEmpty(); }

    QStringList lines() const {
        QStringList out = m_errors;
        for (auto it = m_counters.cbegin(); it != m_counters.cend(); ++it)
            out << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
        out << m_notes;
        for (auto it = m_fonts.cbegin(); it != m_fonts.cend(); ++it)
            out << QStringLiteral("Font substituted: %1 → %2").arg(it.key(), it.value());
        return out;
    }

private:
    QMap<QString, int> m_counters;
    QStringList m_notes;
    QMap<QString, QString> m_fonts;
    QStringList m_errors;
};
