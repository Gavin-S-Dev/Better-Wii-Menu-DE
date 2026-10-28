// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/WiiMenuBannerHeap.h"

namespace WiiForwarder
{
// Check every icon's nested layout/animation structure and conservative working-memory estimate.
// Accepted banners retain their original resources and animations byte for byte.
std::optional<std::vector<u8>> PrepareChannelBanner(std::string_view game_id,
                                                    std::span<const u8> banner,
                                                    size_t heap_size = STOCK_ICON_HEAP);
}  // namespace WiiForwarder
