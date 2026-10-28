// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>
#include "Common/CommonTypes.h"

namespace WiiForwarder
{
constexpr size_t STOCK_ICON_HEAP = 0x8000;
constexpr size_t EXPANDED_ICON_HEAP = 0x20000;
// All five instruction sites must match before any change is made. Only use for System Menu DOLs.
bool CanExpandWiiMenuIconHeaps(std::span<const u8> dol);
bool ExpandWiiMenuIconHeaps(std::span<u8> dol);
size_t GetProfileIconHeapSize();
}  // namespace WiiForwarder
