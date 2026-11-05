// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include <QByteArray>

namespace rtf {

struct Token {
    enum Type { End, GroupStart, GroupEnd, ControlWord, ControlSymbol, Text, Binary, HexByte, Skip };
    Type type = End;
    QByteArray word;      // control word name, or text bytes
    int param = 0;
    bool hasParam = false;
    char symbol = 0;      // control symbol character
    unsigned char hex = 0;
    QByteArray binary;    // \binN payload
};

// Byte-level RTF tokenizer. Never throws; malformed input degrades into
// text or is skipped.
class Tokenizer {
public:
    explicit Tokenizer(const QByteArray &data) : m_data(data) {}
    Token next();
    qint64 position() const { return m_pos; }
    // Skips the rest of the current group (after its opening brace has been
    // consumed), returning false at end of input.
    bool skipGroup();

private:
    Token nextRaw();
    const QByteArray &m_data;
    qint64 m_pos = 0;
};

}  // namespace rtf
