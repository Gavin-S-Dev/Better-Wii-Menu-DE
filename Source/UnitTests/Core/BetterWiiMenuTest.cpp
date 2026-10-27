// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/BetterWiiMenu.h"
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <limits>
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/NandPaths.h"
#include "Core/WiiForwarder.h"
#include "UICommon/ForkProfile.h"
#include "UICommon/ForkRelease.h"
#include "UICommon/GameFile.h"

namespace
{
// Structural archive fixture, not copyrighted game content and never booted by the Wii Menu.
std::vector<u8> Banner()
{
  std::vector<u8> data(0x900);
  const auto put = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i)
      data[offset + i] = static_cast<u8>(value >> (24 - i * 8));
  };
  std::memcpy(data.data() + 0x40, "IMET", 4);
  put(0x600, 0x55aa382d);
  put(0x604, 0x20);
  put(0x620, 0x01000000);
  put(0x628, 3);
  put(0x62c, 1);
  put(0x630, 0xc0);
  put(0x634, 96);
  put(0x638, 12);
  put(0x63c, 0x120);
  put(0x640, 96);
  std::memcpy(data.data() + 0x644, "\0banner.bin\0icon.bin\0", 21);
  for (const size_t offset : {size_t(0x6c0), size_t(0x720)})
  {
    std::memcpy(data.data() + offset, "IMD5", 4);
    put(offset + 4, 64);
    const std::array<u8, 16> digest{0x47, 0xc7, 0x01, 0x66, 0x40, 0x04, 0x3a, 0xf0,
                                    0xcf, 0xc0, 0x4e, 0xde, 0x04, 0x0c, 0x7e, 0xaa};
    std::copy(digest.begin(), digest.end(), data.begin() + offset + 16);
    put(offset + 32, 0x55aa382d);
    put(offset + 36, 32);
    put(offset + 64, 0x01000000);
    put(offset + 72, 1);
  }
  return data;
}

class BetterWiiMenuTest : public testing::Test
{
protected:
  void SetUp() override
  {
    m_path = File::CreateTempDir();
    ASSERT_FALSE(m_path.empty());
    m_previous = File::GetUserPath(D_WIIROOT_IDX);
    File::SetUserPath(D_WIIROOT_IDX, m_path + "/Wii/");
    Write(m_path + "/Wii/title/00000001/00000002/content/title.tmd", "test system menu");
  }
  void TearDown() override
  {
    File::SetUserPath(D_WIIROOT_IDX, m_previous);
    if (!m_path.empty())
      File::DeleteDirRecursively(m_path);
  }
  static void Write(const std::string& path, std::string_view bytes)
  {
    ASSERT_TRUE(File::CreateFullPath(path));
    ASSERT_TRUE(File::WriteStringToFile(path, bytes));
  }
  WiiForwarder::ChannelData Source(unsigned index)
  {
    const auto path = m_path + fmt::format("/games/Game {}.iso", index);
    Write(path, "placeholder");
    return {fmt::format("T{:05}", index), path, Banner(), 0x0000000100000038};
  }
  std::string m_path, m_previous;
};
}  // namespace

TEST(BetterWiiMenuVersion, ComparesForkVersionsNumerically)
{
  EXPECT_EQ(BetterWiiMenu::ParseVersion("v1.0"), BetterWiiMenu::ParseVersion("1.0.0"));
  EXPECT_GT(*BetterWiiMenu::ParseVersion("1.10"), *BetterWiiMenu::ParseVersion("1.9.9"));
  for (const auto* bad :
       {"", "v", "1.", "1.2.", "1.2.3.4", "-1.0", "1.0-beta", "99999999999999999999"})
    EXPECT_FALSE(BetterWiiMenu::ParseVersion(bad)) << bad;
}

TEST(BetterWiiMenuEntry, VirtualMenuIsValidWithoutAnInstalledMenu)
{
  UICommon::GameFile menu(BetterWiiMenu::MENU_PATH);
  EXPECT_TRUE(menu.IsValid());
  EXPECT_TRUE(menu.IsWiiSystemMenu());
  EXPECT_EQ(menu.GetTitleID(), 0x0000000100000002ULL);
  EXPECT_EQ(menu.GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom), "Wii Menu");
}

TEST(BetterWiiMenuCapacity, DoesNotUnderflowAtOrAboveTheMenuLimit)
{
  EXPECT_EQ(WiiForwarder::AvailableSlots(0), 47u);
  EXPECT_EQ(WiiForwarder::AvailableSlots(4), 43u);
  EXPECT_EQ(WiiForwarder::AvailableSlots(47), 0u);
  EXPECT_EQ(WiiForwarder::AvailableSlots(48), 0u);
  EXPECT_EQ(WiiForwarder::AvailableSlots(std::numeric_limits<size_t>::max()), 0u);
}

TEST(BetterWiiMenuBanner, RejectsMissingAndOutOfBoundsArchives)
{
  auto banner = Banner();
  ASSERT_TRUE(WiiForwarder::IsBannerSafe(banner));
  for (size_t length = 0; length < 0x645; ++length)
    EXPECT_FALSE(WiiForwarder::IsBannerSafe(std::span(banner).first(length)));
  banner[0x630] = 0xff;
  EXPECT_FALSE(WiiForwarder::IsBannerSafe(banner));
  banner = Banner();
  banner[0x628] = 0x7f;
  EXPECT_FALSE(WiiForwarder::IsBannerSafe(banner));
  banner.assign(0x600, 0);
  EXPECT_FALSE(WiiForwarder::IsBannerSafe(banner));
}

TEST_F(BetterWiiMenuTest, InstallsAtExactCapacityAndRejectsOverflow)
{
  // Four existing genuine channels consume four slots.
  for (unsigned i = 0; i < 4; ++i)
    Write(m_path + fmt::format("/Wii/title/00010002/{:08x}/content/title.tmd", i),
          "system channel");
  for (unsigned i = 0; i < 43; ++i)
    ASSERT_TRUE(WiiForwarder::InstallChannel(Source(i), true)) << i;
  EXPECT_EQ(WiiForwarder::CountVisibleChannels(), 47u);
  EXPECT_EQ(WiiForwarder::GetInstalledForwarders().size(), 43u);
  EXPECT_FALSE(WiiForwarder::InstallChannel(Source(44), true));
  EXPECT_EQ(WiiForwarder::CountVisibleChannels(), 47u);
}

TEST_F(BetterWiiMenuTest, DeduplicatesAndRelinksWithoutOverwritingAnotherChannel)
{
  auto source = Source(1);
  ASSERT_TRUE(WiiForwarder::InstallChannel(source, true));
  const auto first_path = source.path;
  source.path = m_path + "/replacement.iso";
  Write(source.path, "replacement");
  ASSERT_TRUE(WiiForwarder::InstallChannel(source, true));
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  EXPECT_EQ(WiiForwarder::GetDiscImagePath(id), WiiForwarder::NormalizePath(first_path));
  ASSERT_TRUE(File::Delete(first_path));
  ASSERT_TRUE(WiiForwarder::InstallChannel(source, true));
  EXPECT_EQ(WiiForwarder::GetDiscImagePath(id), WiiForwarder::NormalizePath(source.path));
  auto collision = Source(2);
  const auto collision_id = WiiForwarder::GenerateForwarderTitleID(collision.game_id);
  const auto path = Common::GetTMDFileName(collision_id, Common::FromWhichRoot::Configured);
  Write(path, "genuine channel");
  EXPECT_FALSE(WiiForwarder::InstallChannel(collision, true));
  std::string existing;
  ASSERT_TRUE(File::ReadFileToString(path, existing));
  EXPECT_EQ(existing, "genuine channel");
}

TEST_F(BetterWiiMenuTest, ManualRemovalPersistsAcrossSyncAndCanBeAddedAgain)
{
  auto source = Source(3);
  ASSERT_TRUE(WiiForwarder::InstallChannel(source, true));
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  ASSERT_TRUE(WiiForwarder::UninstallForwarder(id));
  EXPECT_FALSE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  EXPECT_TRUE(WiiForwarder::InstallChannel(source, false));
}

TEST_F(BetterWiiMenuTest, FailedMappingWritePublishesNoChannel)
{
  const auto source = Source(4);
  // A directory occupying the temporary file deterministically forces a write failure.
  ASSERT_TRUE(File::CreateFullPath(WiiForwarder::GetForwarderMappingFilePath() + ".tmp/"));
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  EXPECT_TRUE(WiiForwarder::GetInstalledForwarders().empty());
  EXPECT_EQ(WiiForwarder::CountVisibleChannels(), 0u);
}

TEST_F(BetterWiiMenuTest, RepairsAnInterruptedInstall)
{
  const auto source = Source(5);
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  Write(title + ".installing/content/partial.app", "partial");
  Write(WiiForwarder::GetForwarderMappingFilePath(),
        fmt::format("[Pending]\n{:016x} = {}\n", id, source.path));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_FALSE(File::Exists(title + ".installing"));
  EXPECT_TRUE(WiiForwarder::InstallChannel(source, true));
}

TEST_F(BetterWiiMenuTest, QuarantinesKnownUnsafeBannerAndInvalidMetadata)
{
  auto source = Source(6);
  source.game_id = "SSQE01";
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  source.game_id = "broken";
  source.banner.clear();
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  EXPECT_EQ(WiiForwarder::CountVisibleChannels(), 0u);
}

TEST_F(BetterWiiMenuTest, ImportsSettingsLibraryAndSavesOnceWithoutChangingSource)
{
  const auto original = m_path + "/original";
  const auto fork = m_path + "/fork";
  const std::string config = "[General]\nISOPaths = 1\nISOPath0 = D:/My Games\n"
                             "[Core]\nCPUThread = False\n[AutoUpdate]\nUpdateTrack = dev\n";
  Write(original + "/Config/Dolphin.ini", config);
  Write(original + "/Config/GFX.ini", "[Settings]\nInternalResolution = 3\n");
  Write(original + "/Wii/title/save.bin", "saved game");
  std::string error;
  EXPECT_EQ(UICommon::PrepareForkProfile(original, fork, &error),
            UICommon::ForkProfileResult::Imported)
      << error;
  Common::IniFile imported;
  ASSERT_TRUE(imported.Load(fork + "/Config/Dolphin.ini"));
  std::string path, track;
  EXPECT_TRUE(imported.GetIfExists("General", "ISOPath0", &path));
  EXPECT_EQ(path, "D:/My Games");
  EXPECT_TRUE(imported.GetIfExists("AutoUpdate", "UpdateTrack", &track));
  EXPECT_EQ(track, "fork");
  EXPECT_TRUE(File::Exists(fork + "/Config/GFX.ini"));
  EXPECT_TRUE(File::Exists(fork + "/Wii/title/save.bin"));
  Write(fork + "/Config/GFX.ini", "custom fork setting");
  EXPECT_EQ(UICommon::PrepareForkProfile(original, fork, &error),
            UICommon::ForkProfileResult::Existing);
  std::string unchanged;
  ASSERT_TRUE(File::ReadFileToString(original + "/Config/Dolphin.ini", unchanged));
  EXPECT_EQ(unchanged, config);
  ASSERT_TRUE(File::ReadFileToString(fork + "/Config/GFX.ini", unchanged));
  EXPECT_EQ(unchanged, "custom fork setting");
}

TEST_F(BetterWiiMenuTest, StartsFreshWithoutDolphinAndDoesNotReuseFailedImport)
{
  std::string error;
  EXPECT_EQ(UICommon::PrepareForkProfile(m_path + "/absent", m_path + "/fresh", &error),
            UICommon::ForkProfileResult::Fresh);
  Write(m_path + "/bad.importing/incomplete", "incomplete");
  EXPECT_EQ(UICommon::PrepareForkProfile(m_path + "/absent", m_path + "/bad", &error),
            UICommon::ForkProfileResult::Failed);
  EXPECT_FALSE(File::Exists(m_path + "/bad"));
}

TEST(BetterWiiMenuRelease, AcceptsOnlyStableReleasesFromTheFork)
{
  const std::string valid =
      R"({"tag_name":"v1.2.0","draft":false,"prerelease":false,"html_url":"https://github.com/Gavin-S-Dev/Better-Wii-Menu-DE/releases/tag/v1.2.0"})";
  EXPECT_EQ(UICommon::ParseForkRelease(valid), "v1.2.0");
  EXPECT_FALSE(UICommon::ParseForkRelease("not json"));
  EXPECT_FALSE(UICommon::ParseForkRelease("{}"));
  auto changed = valid;
  changed.replace(changed.find("false"), 5, "true");
  EXPECT_FALSE(UICommon::ParseForkRelease(changed));
  changed = valid;
  changed.replace(changed.rfind("false"), 5, "true");
  EXPECT_FALSE(UICommon::ParseForkRelease(changed));
  changed = valid;
  changed.replace(changed.find("Gavin-S-Dev"), 11, "dolphin-emu");
  EXPECT_FALSE(UICommon::ParseForkRelease(changed));
}

TEST_F(BetterWiiMenuTest, ImportsIntoUnicodePathAndCopiesCustomNand)
{
  const auto source = m_path + "/unicode-source";
  const auto target = m_path + "/fork-\xc3\xa9";
  const auto nand = m_path + "/external-nand";
  Write(nand + "/save.dat", "precious save");
  Write(source + "/Config/Dolphin.ini", "[General]\nNANDRootPath = " + nand + "\n");
  std::string error;
  ASSERT_EQ(UICommon::PrepareForkProfile(source, target, &error),
            UICommon::ForkProfileResult::Imported)
      << error;
  EXPECT_TRUE(File::Exists(target + "/Wii/save.dat"));
  EXPECT_TRUE(File::Exists(nand + "/save.dat"));
  Common::IniFile ini;
  ASSERT_TRUE(ini.Load(target + "/Config/Dolphin.ini"));
  EXPECT_FALSE(ini.Exists("General", "NANDRootPath"));
}

TEST_F(BetterWiiMenuTest, RejectsSystemTitleKeysInForwarderMapping)
{
  Write(WiiForwarder::GetForwarderMappingFilePath(),
        "[Forwarders]\n0000000100000002 = D:/fake.iso\nnot-a-title = D:/fake.iso\n");
  EXPECT_TRUE(WiiForwarder::GetInstalledForwarders().empty());
  EXPECT_FALSE(WiiForwarder::UninstallForwarder(0x0000000100000002));
  EXPECT_TRUE(File::Exists(m_path + "/Wii/title/00000001/00000002/content/title.tmd"));
}

TEST_F(BetterWiiMenuTest, RepairsAnInterruptedRemovalWithoutDeletingSaveData)
{
  const auto source = Source(7);
  ASSERT_TRUE(WiiForwarder::InstallChannel(source, true));
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  Write(title + "/data/save.bin", "preserve");
  Common::IniFile ini;
  ASSERT_TRUE(ini.Load(WiiForwarder::GetForwarderMappingFilePath()));
  ini.GetOrCreateSection("Removing")->Set(fmt::format("{:016x}", id), true);
  ASSERT_TRUE(ini.Save(WiiForwarder::GetForwarderMappingFilePath()));
  ASSERT_TRUE(File::Rename(title + "/content", title + "/content.removing"));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_FALSE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_TRUE(File::Exists(title + "/data/save.bin"));
}

TEST_F(BetterWiiMenuTest, QuarantinesPreviouslyInstalledKnownUnsafeChannel)
{
  const auto id = WiiForwarder::GenerateForwarderTitleID("SSQE01");
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  auto content = Banner();
  const std::string marker = "DFWDold.iso";
  content.insert(content.end(), marker.begin(), marker.end());
  content.push_back(0);
  Write(title + "/content/00000000.app",
        std::string(reinterpret_cast<const char*>(content.data()), content.size()));
  Write(title + "/content/title.tmd", "legacy metadata");
  Write(WiiForwarder::GetForwarderMappingFilePath(),
        fmt::format("[Forwarders]\n{:016x} = old.iso\n", id));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_FALSE(File::Exists(title + "/content/title.tmd"));
  EXPECT_TRUE(File::Exists(title + "/content.quarantined/00000000.app"));
}
