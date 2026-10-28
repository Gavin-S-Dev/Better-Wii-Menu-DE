// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/BetterWiiMenu.h"
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <limits>
#include <mbedtls/md5.h>
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/NandPaths.h"
#include "Core/WiiBanner.h"
#include "Core/WiiForwarder.h"
#include "UICommon/ForkProfile.h"
#include "UICommon/ForkRelease.h"
#include "UICommon/GameFile.h"

namespace
{
using Bytes = std::vector<u8>;
void Put32(Bytes& data, size_t offset, u32 value)
{
  for (size_t i = 0; i < 4; ++i)
    data[offset + i] = static_cast<u8>(value >> (24 - i * 8));
}
void Put16(Bytes& data, size_t offset, u16 value)
{
  data[offset] = static_cast<u8>(value >> 8);
  data[offset + 1] = static_cast<u8>(value);
}
u32 Get32(const Bytes& data, size_t offset)
{
  return (u32(data[offset]) << 24) | (u32(data[offset + 1]) << 16) | (u32(data[offset + 2]) << 8) |
         data[offset + 3];
}
Bytes Wrap(const Bytes& data)
{
  Bytes result(32);
  std::memcpy(result.data(), "IMD5", 4);
  Put32(result, 4, static_cast<u32>(data.size()));
  mbedtls_md5_ret(data.data(), data.size(), result.data() + 16);
  result.insert(result.end(), data.begin(), data.end());
  return result;
}
Bytes Archive(const std::vector<std::pair<std::string, Bytes>>& files)
{
  Bytes result(32 + (files.size() + 1) * 12);
  Put32(result, 0, 0x55aa382d);
  Put32(result, 4, 32);
  Put32(result, 32, 0x01000000);
  Put32(result, 40, static_cast<u32>(files.size() + 1));
  const size_t strings = result.size();
  result.push_back(0);
  for (size_t i = 0; i < files.size(); ++i)
  {
    Put32(result, 44 + i * 12, static_cast<u32>(result.size() - strings));
    result.insert(result.end(), files[i].first.begin(), files[i].first.end());
    result.push_back(0);
  }
  Put32(result, 8, static_cast<u32>(result.size() - 32));
  result.resize((result.size() + 31) & ~size_t{31});
  Put32(result, 12, static_cast<u32>(result.size()));
  for (size_t i = 0; i < files.size(); ++i)
  {
    result.resize((result.size() + 31) & ~size_t{31});
    Put32(result, 48 + i * 12, static_cast<u32>(result.size()));
    Put32(result, 52 + i * 12, static_cast<u32>(files[i].second.size()));
    result.insert(result.end(), files[i].second.begin(), files[i].second.end());
  }
  return result;
}
Bytes Section(const char* magic, size_t size)
{
  Bytes result(size);
  std::memcpy(result.data(), magic, 4);
  Put32(result, 4, static_cast<u32>(size));
  return result;
}
Bytes LayoutFile(const char* magic, const std::vector<Bytes>& sections)
{
  Bytes result(16);
  std::memcpy(result.data(), magic, 4);
  Put16(result, 4, 0xfeff);
  Put16(result, 6, 8);
  Put16(result, 12, 16);
  Put16(result, 14, static_cast<u16>(sections.size()));
  for (const auto& section : sections)
    result.insert(result.end(), section.begin(), section.end());
  Put32(result, 8, static_cast<u32>(result.size()));
  return result;
}
// Minimal layout/animation fixtures with a decorative pane and two logo animation bindings.
// These are generated metadata, contain no game textures, and are never rendered.
Bytes CompatibilityBanner(bool compressed = false, bool bad_distance = false)
{
  auto materials = Section("mat1", 24 + 3 * 64);
  Put16(materials, 8, 3);
  for (size_t i = 0; i < 3; ++i)
    Put32(materials, 12 + i * 4, static_cast<u32>(24 + i * 64));
  auto pane = [](const char* name, u16 material) {
    auto pic = Section("pic1", 128);
    std::memcpy(pic.data() + 12, name, std::strlen(name));
    Put16(pic, 92, material);
    return pic;
  };
  auto layout =
      LayoutFile("RLYT", {Section("lyt1", 20), Section("txl1", 12), materials, Section("pan1", 76),
                          Section("pas1", 8), pane("bg", 0), pane("Picture_00", 1),
                          pane("Picture_61", 2), Section("pae1", 8), Section("grp1", 28)});
  auto pai = Section("pai1", 32 + 3 * 24);
  Put16(pai, 8, 420);
  pai[10] = 1;
  Put16(pai, 14, 3);
  Put32(pai, 16, 20);
  for (size_t i = 0; i < 3; ++i)
  {
    const size_t offset = 32 + i * 24;
    Put32(pai, 20 + i * 4, static_cast<u32>(offset));
    std::memcpy(pai.data() + offset, i == 0 ? "Picture_00" : "Picture_61", 10);
  }
  pai[32 + 2 * 24 + 21] = 1;
  Bytes icon = Archive({{"icon.brlyt", layout}, {"icon.brlan", LayoutFile("RLAN", {pai})}});
  const size_t unpacked_size = icon.size();
  if (compressed)
  {
    Bytes encoded{'L',
                  'Z',
                  '7',
                  '7',
                  0x10,
                  static_cast<u8>(icon.size()),
                  static_cast<u8>(icon.size() >> 8),
                  static_cast<u8>(icon.size() >> 16)};
    for (size_t pos = 0; pos < icon.size(); pos += 8)
    {
      encoded.push_back(0);
      const size_t end = std::min(pos + 8, icon.size());
      encoded.insert(encoded.end(), icon.begin() + pos, icon.begin() + end);
    }
    if (bad_distance)
      encoded[8] = 0x80;  // Backreference before any literal has been emitted.
    icon = std::move(encoded);
  }
  Bytes result(0x600);
  std::memcpy(result.data() + 0x40, "IMET", 4);
  Put32(result, 0x4c, static_cast<u32>(unpacked_size));
  const auto outer = Archive({{"banner.bin", Wrap(Bytes(64, 1))},
                              {"icon.bin", Wrap(icon)},
                              {"sound.bin", Wrap(Bytes(64, 2))}});
  mbedtls_md5_ret(result.data(), result.size(), result.data() + 0x5f0);
  result.insert(result.end(), outer.begin(), outer.end());
  return result;
}

Bytes Member(const Bytes& banner, size_t index)
{
  const size_t node = 0x600 + 44 + index * 12;
  const size_t offset = 0x600 + Get32(banner, node + 4);
  const size_t length = Get32(banner, node + 8);
  return Bytes(banner.begin() + offset, banner.begin() + offset + length);
}

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
  WiiForwarder::ChannelData LegacyMarioParty9()
  {
    auto source = Source(9);
    source.game_id = "SSQE01";
    source.banner = CompatibilityBanner();
    EXPECT_TRUE(WiiForwarder::InstallChannel(source, true));
    const auto title = Common::GetTitlePath(WiiForwarder::GenerateForwarderTitleID(source.game_id),
                                            Common::FromWhichRoot::Configured);
    auto raw = source.banner;
    const std::string marker = "DFWD" + source.path;
    raw.insert(raw.end(), marker.begin(), marker.end());
    raw.push_back(0);
    std::string metadata;
    EXPECT_TRUE(File::ReadFileToString(title + "/content/title.tmd", metadata));
    Bytes tmd(metadata.begin(), metadata.end());
    Put32(tmd, tmd.size() - 28, 0);
    Put32(tmd, tmd.size() - 24, static_cast<u32>(raw.size()));
    const auto hash = Common::SHA1::CalculateDigest(raw.data(), raw.size());
    std::copy(hash.begin(), hash.end(), tmd.end() - 20);
    Write(title + "/content/00000000.app",
          std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
    Write(title + "/content/title.tmd",
          std::string(reinterpret_cast<const char*>(tmd.data()), tmd.size()));
    Write(title + "/data/save.bin", "preserve");
    return source;
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

TEST_F(BetterWiiMenuTest, InstallsMarioParty9WithACompatibleIconAndRejectsMalformedBanners)
{
  auto source = Source(6);
  source.game_id = "SSQE01";
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  source.banner = CompatibilityBanner();
  EXPECT_TRUE(WiiForwarder::InstallChannel(source, true));
  source.game_id = "broken";
  source.banner.clear();
  EXPECT_FALSE(WiiForwarder::InstallChannel(source, true));
  EXPECT_EQ(WiiForwarder::CountVisibleChannels(), 1u);
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

TEST_F(BetterWiiMenuTest, UpgradesLegacyMarioParty9AndPreservesSavesAndManualRemoval)
{
  const auto source = LegacyMarioParty9();
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  ASSERT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_TRUE(File::Exists(title + "/content/title.tmd"));
  EXPECT_TRUE(File::Exists(title + "/content.before-icon-fix/00000000.app"));
  EXPECT_TRUE(File::Exists(title + "/data/save.bin"));
  EXPECT_TRUE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  // Explicit removal also cancels an upgrade whose final mapping save was interrupted.
  Common::IniFile ini;
  ASSERT_TRUE(ini.Load(WiiForwarder::GetForwarderMappingFilePath()));
  ini.GetOrCreateSection("IconFix")->Set(fmt::format("{:016x}", id), true);
  ASSERT_TRUE(ini.Save(WiiForwarder::GetForwarderMappingFilePath()));
  EXPECT_TRUE(WiiForwarder::UninstallForwarder(id));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_FALSE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_TRUE(File::Exists(title + "/data/save.bin"));
}

TEST_F(BetterWiiMenuTest, RestoresPreviouslyQuarantinedMarioParty9WithoutUndoingLaterRemoval)
{
  const auto source = LegacyMarioParty9();
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  ASSERT_TRUE(File::Rename(title + "/content", title + "/content.quarantined"));
  Write(WiiForwarder::GetForwarderMappingFilePath(),
        fmt::format("[Excluded]\n{:016x} = True\n", id));
  ASSERT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_TRUE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_TRUE(File::Exists(title + "/content.quarantined/00000000.app"));
  EXPECT_TRUE(File::Exists(title + "/data/save.bin"));
  ASSERT_TRUE(WiiForwarder::UninstallForwarder(id));
  EXPECT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_FALSE(WiiForwarder::IsForwarderInstalled(source.path));
}

TEST_F(BetterWiiMenuTest, RecoversInterruptedMarioParty9IconPublication)
{
  const auto source = LegacyMarioParty9();
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  ASSERT_TRUE(File::Rename(title + "/content", title + "/content.before-icon-fix"));
  Write(title + "/content.icon-fix/partial.app", "partial");
  Write(WiiForwarder::GetForwarderMappingFilePath(),
        fmt::format("[IconFix]\n{:016x} = True\n", id));
  ASSERT_TRUE(WiiForwarder::RepairChannels());
  EXPECT_TRUE(WiiForwarder::IsForwarderInstalled(source.path));
  EXPECT_TRUE(File::Exists(title + "/data/save.bin"));
  EXPECT_FALSE(File::Exists(title + "/content.icon-fix"));
}

TEST_F(BetterWiiMenuTest, DoesNotReplaceUnrelatedActiveContentWithAQuarantinedChannel)
{
  const auto source = LegacyMarioParty9();
  const auto id = WiiForwarder::GenerateForwarderTitleID(source.game_id);
  const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
  ASSERT_TRUE(File::Rename(title + "/content", title + "/content.quarantined"));
  Write(title + "/content/title.tmd", "unrelated");
  EXPECT_FALSE(WiiForwarder::RepairChannels());
  std::string content;
  ASSERT_TRUE(File::ReadFileToString(title + "/content/title.tmd", content));
  EXPECT_EQ(content, "unrelated");
}

TEST(BetterWiiMenuBanner, SimplifiesOnlyTheIconAndKeepsTheFullPreviewAndSound)
{
  for (bool compressed : {false, true})
  {
    const auto raw = CompatibilityBanner(compressed);
    const auto fixed = WiiForwarder::PrepareChannelBanner("SSQE01", raw);
    ASSERT_TRUE(fixed);
    EXPECT_NE(*fixed, raw);
    EXPECT_EQ(Member(*fixed, 0), Member(raw, 0));
    EXPECT_EQ(Member(*fixed, 2), Member(raw, 2));
    EXPECT_TRUE(WiiForwarder::IsBannerSafe(*fixed));
    EXPECT_EQ(WiiForwarder::PrepareChannelBanner("SSQE01", *fixed), fixed);
    EXPECT_EQ(WiiForwarder::PrepareChannelBanner("RMCE01", raw), raw);
  }
}

TEST(BetterWiiMenuBanner, RejectsBrokenCompressionAndTruncatedMarioParty9Input)
{
  const auto raw = CompatibilityBanner(true);
  for (size_t size = 0; size < raw.size(); ++size)
    EXPECT_FALSE(WiiForwarder::PrepareChannelBanner("SSQE01", std::span(raw).first(size)));
  EXPECT_TRUE(WiiForwarder::IsBannerSafe(CompatibilityBanner(true, true)));
  EXPECT_FALSE(WiiForwarder::PrepareChannelBanner("SSQE01", CompatibilityBanner(true, true)));
}
