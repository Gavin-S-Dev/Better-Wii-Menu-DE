// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "UICommon/ForkProfile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Common/IniFile.h"

namespace UICommon
{
ForkProfileResult PrepareForkProfile(const std::string& source, const std::string& destination,
                                     std::string* error)
{
  namespace fs = std::filesystem;
  const auto to_path = [](const std::string& path) {
    return fs::path(std::u8string(path.begin(), path.end()));
  };
  const auto src = to_path(source);
  const auto dst = to_path(destination);
  const auto staging = to_path(destination + ".importing");
  try
  {
    // An existing destination is authoritative, even if it was deliberately created empty.
    if (fs::exists(dst))
      return ForkProfileResult::Existing;
    fs::create_directories(dst.parent_path());
    if (fs::exists(staging))
      throw std::runtime_error("An unfinished import exists at " + destination + ".importing");
    fs::create_directory(staging);
    const bool importing = fs::is_regular_file(src / "Config" / "Dolphin.ini");
    if (importing)
    {
      for (const auto* name : {"Config", "GameSettings", "GC", "Wii", "Load", "ResourcePacks"})
      {
        if (!fs::is_directory(src / name))
          continue;
        fs::create_directory(staging / name);
        for (const auto& entry : fs::recursive_directory_iterator(src / name))
        {
          // Refuse links: an import must not create paths back into the original profile.
          if (entry.is_symlink())
            throw std::runtime_error(
                "Profile contains a symbolic link; use an explicit -u profile.");
          const auto target = staging / fs::relative(entry.path(), src);
          if (entry.is_directory())
            fs::create_directories(target);
          else if (entry.is_regular_file())
            fs::copy_file(entry.path(), target);
        }
      }
      Common::IniFile config;
      const auto utf8_path = (staging / "Config" / "Dolphin.ini").u8string();
      const std::string config_path(utf8_path.begin(), utf8_path.end());
      if (!config.Load(config_path))
        throw std::runtime_error("Cannot read imported settings.");
      std::string nand;
      config.GetOrCreateSection("General")->Get("NANDRootPath", &nand, "");
      if (!nand.empty() && fs::weakly_canonical(to_path(nand)) != fs::weakly_canonical(src / "Wii"))
      {
        // Honor a custom NAND by copying it, never by sharing its writable path.
        if (!fs::is_directory(to_path(nand)))
          throw std::runtime_error("The configured Wii NAND is unavailable: " + nand);
        fs::remove_all(staging / "Wii");
        fs::copy(to_path(nand), staging / "Wii",
                 fs::copy_options::recursive | fs::copy_options::skip_symlinks);
      }
      config.DeleteKey("General", "NANDRootPath");
      // Rebase other user-folder paths, including custom GC cards and texture paths.
      for (auto& section : config.GetSections())
      {
        for (const auto& [key, value] : section.GetValues())
        {
          std::string normalized = value;
          std::replace(normalized.begin(), normalized.end(), '\\', '/');
          std::string prefix = source;
          std::replace(prefix.begin(), prefix.end(), '\\', '/');
          while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();
          if (normalized.starts_with(prefix + "/"))
            config.GetOrCreateSection(section.GetName())
                ->Set(key, destination + normalized.substr(prefix.size()));
        }
      }
      config.GetOrCreateSection("AutoUpdate")->Set("UpdateTrack", std::string("fork"));
      config.DeleteKey("AutoUpdate", "HashOverride");
      // Legacy INIs did not always contain this setting.
      config.GetOrCreateSection("Core")->Set("WiiForwarderAutoSync", true);
      if (!config.Save(config_path))
        throw std::runtime_error("Cannot save migrated settings.");
    }
    std::ofstream marker(staging / "fork-profile.txt");
    marker << "Better Wii Menu DE profile v1\n" << (importing ? source : "Fresh profile") << '\n';
    marker.close();
    if (!marker)
      throw std::runtime_error("Cannot save the profile marker.");
    fs::rename(staging, dst);
    return importing ? ForkProfileResult::Imported : ForkProfileResult::Fresh;
  }
  catch (const std::exception& e)
  {
    if (error)
      *error = e.what();
    return ForkProfileResult::Failed;
  }
}
}  // namespace UICommon
