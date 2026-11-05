// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "flow/FlowModel.h"

#include <QByteArray>

namespace rtf {

// Tolerant RTF 1.9 reader producing the shared FlowModel. Never throws:
// unbalanced braces, unknown control words and truncated files are handled
// by rendering as much as was parsed.
flow::Document import(const QByteArray &data);

}  // namespace rtf
