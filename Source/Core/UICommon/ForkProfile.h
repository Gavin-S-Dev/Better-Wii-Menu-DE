// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace UICommon
{
enum class ForkProfileResult
{
  Existing,
  Fresh,
  Imported,
  Failed
};
// Copies an existing user's data once. Explicit -u and portable profiles bypass migration.
// The source is never modified; a failed staging directory is retained for diagnosis.
ForkProfileResult PrepareForkProfile(const std::string& source, const std::string& destination,
                                     std::string* error);
}  // namespace UICommon
