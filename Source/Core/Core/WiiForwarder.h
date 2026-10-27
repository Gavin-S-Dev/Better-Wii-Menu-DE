// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace WiiForwarder
{
constexpr u32 FORWARDER_TITLE_TYPE = 0x00010001;
constexpr u32 FORWARDER_MAGIC = 0x44465744;  // "DFWD"

std::string NormalizePath(const std::string& path);
size_t AvailableSlots(size_t visible_channels);
size_t CountVisibleChannels();
bool IsBannerSafe(std::span<const u8> data);
bool RepairChannels();

u64 GenerateForwarderTitleID(const std::string& game_id);
bool IsForwarderTitle(u64 title_id);
bool IsForwarderInstalled(const std::string& disc_image_path);
std::optional<std::string> GetDiscImagePath(u64 forwarder_title_id);
std::map<u64, std::string> GetInstalledForwarders();
std::string GetForwarderMappingFilePath();

struct ChannelData
{
  std::string game_id;
  std::string path;
  std::vector<u8> banner;
  u64 ios;
};
// Installs extracted metadata through the transactional path used by disc scanning.
bool InstallChannel(const ChannelData& source, bool silent = false);

// When silent is true, errors are logged but no popup dialogs are shown.
bool InstallForwarder(const std::string& disc_image_path, bool silent = false);
bool UninstallForwarder(u64 forwarder_title_id);

// Called from ES when a forwarder channel is launched. The host (DolphinQt)
// checks this after stopping emulation and reboots with the disc image.
void SetPendingForwarderBoot(const std::string& disc_image_path);
std::string ConsumePendingForwarderBoot();
bool HasPendingForwarderBoot();

}  // namespace WiiForwarder
