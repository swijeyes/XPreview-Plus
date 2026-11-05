// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#include "RtfTokenizer.h"

#include "core/Limits.h"

namespace rtf {

namespace {

inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Token Tokenizer::next()
{
    // Iterative: malformed escapes are skipped in this loop, never by
    // recursion (a file of "\\'zz" repeated must not exhaust the stack).
    for (;;) {
        Token t = nextRaw();
        if (t.type != Token::Skip) return t;
    }
}

Token Tokenizer::nextRaw()
{
    Token t;
    const qint64 n = m_data.size();
    const char *d = m_data.constData();
    // CR/LF outside of control words are ignored by RTF readers.
    while (m_pos < n && (d[m_pos] == '\r' || d[m_pos] == '\n')) ++m_pos;
    if (m_pos >= n) return t;

    const char c = d[m_pos];
    if (c == '{') {
        ++m_pos;
        t.type = Token::GroupStart;
        return t;
    }
    if (c == '}') {
        ++m_pos;
        t.type = Token::GroupEnd;
        return t;
    }
    if (c == '\\') {
        ++m_pos;
        if (m_pos >= n) return t;
        const char s = d[m_pos];
        if (isAlpha(s)) {
            const qint64 start = m_pos;
            while (m_pos < n && isAlpha(d[m_pos]) && m_pos - start < 32) ++m_pos;
            t.type = Token::ControlWord;
            t.word = QByteArray(d + start, int(m_pos - start));
            if (m_pos < n && (d[m_pos] == '-' || isDigit(d[m_pos]))) {
                bool neg = false;
                if (d[m_pos] == '-') {
                    neg = true;
                    ++m_pos;
                }
                long long v = 0;
                int digits = 0;
                while (m_pos < n && isDigit(d[m_pos])) {
                    if (digits < 10) v = v * 10 + (d[m_pos] - '0');
                    ++digits;
                    ++m_pos;
                }
                v = qBound(-2147483647LL, neg ? -v : v, 2147483647LL);
                t.param = int(v);
                t.hasParam = digits > 0;
            }
            if (m_pos < n && d[m_pos] == ' ') ++m_pos;  // delimiter
            if (t.word == "bin" && t.hasParam) {
                const qint64 len = qBound<qint64>(0, qint64(t.param), qMin<qint64>(n - m_pos, qint64(Limits::kMaxRtfBinBytes)));
                t.type = Token::Binary;
                t.binary = m_data.mid(m_pos, int(len));
                m_pos += t.param > 0 ? qMin<qint64>(t.param, n - m_pos) : 0;
            }
            return t;
        }
        if (s == '\'') {
            ++m_pos;
            const int h1 = m_pos < n ? hexValue(d[m_pos]) : -1;
            const int h2 = m_pos + 1 < n ? hexValue(d[m_pos + 1]) : -1;
            if (h1 >= 0 && h2 >= 0) {
                m_pos += 2;
                t.type = Token::HexByte;
                t.hex = (unsigned char)(h1 * 16 + h2);
                return t;
            }
            t.type = Token::Skip;  // malformed: ignore
            return t;
        }
        ++m_pos;
        if (s == '\r' || s == '\n') {
            // "\<newline>" is an alias for \par.
            t.type = Token::ControlWord;
            t.word = "par";
            return t;
        }
        t.type = Token::ControlSymbol;
        t.symbol = s;
        return t;
    }

    // Plain text run up to the next special character.
    const qint64 start = m_pos;
    while (m_pos < n) {
        const char x = d[m_pos];
        if (x == '\\' || x == '{' || x == '}' || x == '\r' || x == '\n') break;
        ++m_pos;
    }
    t.type = Token::Text;
    t.word = QByteArray(d + start, int(m_pos - start));
    return t;
}

bool Tokenizer::skipGroup()
{
    int depth = 1;
    const qint64 n = m_data.size();
    const char *d = m_data.constData();
    while (m_pos < n) {
        const char c = d[m_pos];
        if (c == '\\') {
            // Skip escaped braces/backslashes and \bin payloads correctly.
            if (m_pos + 1 < n && (d[m_pos + 1] == '{' || d[m_pos + 1] == '}' || d[m_pos + 1] == '\\')) {
                m_pos += 2;
                continue;
            }
            if (m_pos + 4 < n && qstrncmp(d + m_pos + 1, "bin", 3) == 0 && isDigit(d[m_pos + 4])) {
                Token t = next();
                Q_UNUSED(t);
                continue;
            }
            ++m_pos;
            continue;
        }
        ++m_pos;
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return true;
        }
    }
    return false;
}

}  // namespace rtf
