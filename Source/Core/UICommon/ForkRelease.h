// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace UICommon
{
std::optional<std::string> ParseForkRelease(std::string_view response);
}
