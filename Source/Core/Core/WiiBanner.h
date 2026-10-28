// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace WiiForwarder
{
// Preserve disc banners except for MP9's complex icon, which crashes the channel grid.
// Produces a smaller animated logo/background icon from the user's own disc resources.
std::optional<std::vector<u8>> PrepareChannelBanner(std::string_view game_id,
                                                    std::span<const u8> banner);
}  // namespace WiiForwarder
