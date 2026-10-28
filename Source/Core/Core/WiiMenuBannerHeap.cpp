// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/WiiMenuBannerHeap.h"

#include <array>
#include <fmt/format.h>
#include <optional>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/NandPaths.h"
#include "Core/CommonTitles.h"
#include "Core/IOS/ES/Formats.h"

namespace WiiForwarder
{
namespace
{
using View = std::span<const u8>;
u32 Read(View data, size_t offset)
{
  return (u32(data[offset]) << 24) | (u32(data[offset + 1]) << 16) | (u32(data[offset + 2]) << 8) |
         data[offset + 3];
}
struct Word
{
  u32 value, mask = 0xffffffff;
};
struct Patch
{
  size_t offset;
  u32 word;
};
std::optional<std::vector<Patch>> Plan(View dol)
{
  if (dol.size() < 0x100 || dol.size() > 16 * 1024 * 1024)
    return {};
  // Match the icon layout heap, both per-icon parent-frame allocations, and the 49-block pool.
  // Relocatable branch/string addresses are masked; allocation sizes and the surrounding operations
  // are checked. A partial or ambiguous match never changes the executable.
  const std::array<Word, 12> icon{{{0x80a3002c},
                                   {0x3fa00000, 0xffff0000},
                                   {0x7c7e1b78},
                                   {0x3c800001},
                                   {0x3bbd0000, 0xffff0000},
                                   {0xc3c20000, 0xffff0000},
                                   {0x80630010},
                                   {0x38848000},
                                   {0x80a500a0},
                                   {0x38fd0000, 0xffff0000},
                                   {0x38cd0000, 0xffff0000},
                                   {0x48000001, 0xfc000003}}};
  const std::array<Word, 5> parent{{{0x3c600002},
                                    {0x8080016c, 0xffe0ffff},
                                    {0x386312b8},
                                    {0x38a00002},
                                    {0x48000001, 0xfc000003}}};
  const std::array<Word, 13> pool{{{0x3fe00002},
                                   {0x387f12b8},
                                   {0x38800031},
                                   {0x38a00020},
                                   {0x48000001, 0xfc000003},
                                   {0x3ca00000, 0xffff0000},
                                   {0x389f12b8},
                                   {0x38a50000, 0xffff0000},
                                   {0x38c00020},
                                   {0x80a50028},
                                   {0x38e00002},
                                   {0x48000001, 0xfc000003},
                                   {0x9060016c, 0xffe0ffff}}};
  std::array<std::vector<size_t>, 3> matches;
  const std::array<std::span<const Word>, 3> patterns{icon, parent, pool};
  // The menu's native PPC code is carried in a DOL data section behind its launch stub.
  for (size_t section = 0; section < 18; ++section)
  {
    const size_t offset = Read(dol, section * 4);
    const size_t size = Read(dol, 0x90 + section * 4);
    const u32 address = Read(dol, 0x48 + section * 4);
    if (!size)
      continue;
    if (offset < 0x100 || offset > dol.size() || size > dol.size() - offset ||
        address < 0x80000000 || u64(address) + size > 0x94000000)
      return {};
    for (size_t p = 0; p < patterns.size(); ++p)
    {
      const auto pattern = patterns[p];
      for (size_t pos = offset; pattern.size() * 4 <= offset + size - pos; pos += 4)
      {
        bool same = true;
        for (size_t i = 0; i < pattern.size(); ++i)
          same &= (Read(dol, pos + i * 4) & pattern[i].mask) == pattern[i].value;
        if (same)
          matches[p].push_back(pos);
      }
    }
  }
  if (matches[0].size() != 1 || matches[1].size() != 2 || matches[2].size() != 1)
    return {};
  return std::vector<Patch>{{matches[0][0] + 12, 0x3c800002},
                            {matches[0][0] + 28, 0x38840000},
                            {matches[1][0], 0x3c600004},
                            {matches[1][1], 0x3c600004},
                            {matches[2][0], 0x3fe00004}};
}
}  // namespace

bool CanExpandWiiMenuIconHeaps(View dol)
{
  return Plan(dol).has_value();
}
bool ExpandWiiMenuIconHeaps(std::span<u8> dol)
{
  const auto plan = Plan(dol);
  if (!plan)
    return false;
  for (const auto& patch : *plan)
    for (size_t i = 0; i < 4; ++i)
      dol[patch.offset + i] = static_cast<u8>(patch.word >> (24 - i * 8));
  return true;
}

size_t GetProfileIconHeapSize()
{
  const auto root = Common::FromWhichRoot::Configured;
  std::string metadata;
  if (!File::ReadFileToString(Common::GetTMDFileName(Titles::SYSTEM_MENU, root), metadata))
    return STOCK_ICON_HEAP;
  IOS::ES::TMDReader tmd(std::vector<u8>(metadata.begin(), metadata.end()));
  IOS::ES::Content boot;
  if (!tmd.IsValid() || !tmd.GetContent(tmd.GetBootIndex(), &boot) || (boot.type & 0x8000))
    return STOCK_ICON_HEAP;
  const auto path =
      Common::GetTitleContentPath(Titles::SYSTEM_MENU, root) + fmt::format("/{:08x}.app", boot.id);
  if (File::GetSize(path) > 16 * 1024 * 1024)
    return STOCK_ICON_HEAP;
  std::string binary;
  if (!File::ReadFileToString(path, binary))
    return STOCK_ICON_HEAP;
  return CanExpandWiiMenuIconHeaps(
             View(reinterpret_cast<const u8*>(binary.data()), binary.size())) ?
             EXPANDED_ICON_HEAP :
             STOCK_ICON_HEAP;
}
}  // namespace WiiForwarder
