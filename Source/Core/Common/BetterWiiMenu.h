// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string_view>

namespace BetterWiiMenu
{
inline constexpr char VERSION[] = "1.3.1";
inline constexpr char RELEASES_URL[] = "https://github.com/Gavin-S-Dev/Better-Wii-Menu-DE/releases";
inline constexpr char RELEASES_API[] =
    "https://api.github.com/repos/Gavin-S-Dev/Better-Wii-Menu-DE/releases/latest";
inline constexpr char MENU_PATH[] = "dolphin://wii-menu";

inline std::optional<std::array<unsigned, 3>> ParseVersion(std::string_view version)
{
  if (version.starts_with('v'))
    version.remove_prefix(1);
  std::array<unsigned, 3> result{};
  for (unsigned i = 0; i < result.size(); ++i)
  {
    const auto dot = version.find('.');
    const auto part = version.substr(0, dot);
    if (part.empty())
      return std::nullopt;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), result[i]);
    if (error != std::errc{} || end != part.data() + part.size())
      return std::nullopt;
    if (dot == std::string_view::npos)
      return result;
    version.remove_prefix(dot + 1);
  }
  return std::nullopt;
}
}  // namespace BetterWiiMenu
