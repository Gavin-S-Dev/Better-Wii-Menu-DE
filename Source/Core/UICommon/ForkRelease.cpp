// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "UICommon/ForkRelease.h"
#include <picojson.h>
#include "Common/BetterWiiMenu.h"

namespace UICommon
{
std::optional<std::string> ParseForkRelease(std::string_view response)
{
  picojson::value document;
  if (!picojson::parse(document, std::string(response)).empty() || !document.is<picojson::object>())
    return std::nullopt;
  const auto& object = document.get<picojson::object>();
  for (const auto* field : {"draft", "prerelease"})
  {
    const auto found = object.find(field);
    if (found == object.end() || !found->second.is<bool>() || found->second.get<bool>())
      return std::nullopt;
  }
  const auto tag = object.find("tag_name"), url = object.find("html_url");
  if (tag == object.end() || url == object.end() || !tag->second.is<std::string>() ||
      !url->second.is<std::string>())
    return std::nullopt;
  const auto& version = tag->second.get<std::string>();
  if (!BetterWiiMenu::ParseVersion(version) ||
      !url->second.get<std::string>().starts_with(std::string(BetterWiiMenu::RELEASES_URL) +
                                                  "/tag/"))
    return std::nullopt;
  return version;
}
}  // namespace UICommon
