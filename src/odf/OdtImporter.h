// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sanjeeva Wijeyesakere

#pragma once

#include "flow/FlowModel.h"

namespace odf {

// Imports ODT/OTT packages and flat FODT files into the FlowModel. On a
// partial failure, whatever was parsed is returned with an error in the
// report; if nothing could be parsed the package thumbnail is used.
flow::Document importOdt(const QString &path, bool flat, QString *error);

}  // namespace odf
