// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiBanner.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mbedtls/md5.h>
#include <string>

#include "Common/Align.h"
#include "Core/WiiForwarder.h"

namespace WiiForwarder
{
namespace
{
using Bytes = std::vector<u8>;
using View = std::span<const u8>;
constexpr size_t MAX_SIZE = 16 * 1024 * 1024;

u32 Read32(View data, size_t offset)
{
  return (u32(data[offset]) << 24) | (u32(data[offset + 1]) << 16) | (u32(data[offset + 2]) << 8) |
         data[offset + 3];
}
u16 Read16(View data, size_t offset)
{
  return (u16(data[offset]) << 8) | data[offset + 1];
}
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
bool Magic(View data, size_t offset, std::string_view magic)
{
  return offset <= data.size() && magic.size() <= data.size() - offset &&
         std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}
void Append(Bytes& data, View addition)
{
  data.insert(data.end(), addition.begin(), addition.end());
}
std::string Name(View data)
{
  const auto end = std::find(data.begin(), data.end(), 0);
  return std::string(data.begin(), end);
}

struct Entry
{
  std::string name;
  size_t node, offset, size;
};
std::optional<std::vector<Entry>> Entries(View archive)
{
  if (archive.size() < 32 || archive.size() > MAX_SIZE || Read32(archive, 0) != 0x55aa382d)
    return {};
  const size_t root = Read32(archive, 4);
  if (root > archive.size() - 12)
    return {};
  const size_t count = Read32(archive, root + 8);
  if (count == 0 || count > 4096 || count > (archive.size() - root) / 12)
    return {};
  const size_t strings = root + count * 12;
  const size_t first = Read32(archive, 12);
  if (first < strings || first > archive.size())
    return {};
  std::vector<Entry> result;
  for (size_t i = 1; i < count; ++i)
  {
    const size_t node = root + i * 12;
    const size_t name_offset = Read32(archive, node) & 0xffffff;
    if (name_offset >= first - strings)
      return {};
    const auto text = archive.subspan(strings + name_offset, first - strings - name_offset);
    if (std::find(text.begin(), text.end(), 0) == text.end())
      return {};
    if (archive[node] == 1)
      continue;
    if (archive[node] != 0)
      return {};
    const size_t offset = Read32(archive, node + 4), size = Read32(archive, node + 8);
    if (offset < first || offset > archive.size() || size > archive.size() - offset)
      return {};
    result.push_back({Name(text), node, offset, size});
  }
  return result;
}
std::optional<Bytes> Rebuild(View archive, const std::map<std::string, Bytes>& replacements)
{
  const auto entries = Entries(archive);
  if (!entries || entries->empty())
    return {};
  const size_t first = Read32(archive, 12);
  Bytes result(archive.begin(), archive.begin() + first);
  for (const auto& entry : *entries)
  {
    result.resize(Common::AlignUp(result.size(), size_t{32}));
    const auto replacement = replacements.find(entry.name);
    const View payload = replacement == replacements.end() ?
                             archive.subspan(entry.offset, entry.size) :
                             View(replacement->second);
    if (payload.size() > MAX_SIZE - result.size())
      return {};
    Put32(result, entry.node + 4, static_cast<u32>(result.size()));
    Put32(result, entry.node + 8, static_cast<u32>(payload.size()));
    Append(result, payload);
  }
  return result;
}
std::optional<Bytes> Unpack(View wrapped)
{
  if (wrapped.size() < 32 || !Magic(wrapped, 0, "IMD5"))
    return {};
  View data = wrapped.subspan(32);
  if (data.size() >= 4 && Read32(data, 0) == 0x55aa382d)
    return Bytes(data.begin(), data.end());
  if (Magic(data, 0, "LZ77"))
    data = data.subspan(4);
  if (data.size() < 4 || data[0] != 0x10)
    return {};
  const size_t size = data[1] | (size_t(data[2]) << 8) | (size_t(data[3]) << 16);
  if (size < 32 || size > 2 * 1024 * 1024)
    return {};
  Bytes result;
  result.reserve(size);
  size_t pos = 4;
  while (result.size() < size)
  {
    if (pos >= data.size())
      return {};
    const u8 flags = data[pos++];
    for (int bit = 7; bit >= 0 && result.size() < size; --bit)
    {
      if ((flags & (1 << bit)) == 0)
      {
        if (pos >= data.size())
          return {};
        result.push_back(data[pos++]);
      }
      else
      {
        if (data.size() - pos < 2)
          return {};
        const size_t length = (data[pos] >> 4) + 3;
        const size_t distance = ((size_t(data[pos]) & 15) << 8 | data[pos + 1]) + 1;
        pos += 2;
        if (distance > result.size() || length > size - result.size())
          return {};
        for (size_t i = 0; i < length; ++i)
          result.push_back(result[result.size() - distance]);
      }
    }
  }
  return result;
}
Bytes Wrap(View data)
{
  Bytes result(32);
  std::memcpy(result.data(), "IMD5", 4);
  Put32(result, 4, static_cast<u32>(data.size()));
  mbedtls_md5_ret(data.data(), data.size(), result.data() + 16);
  Append(result, data);
  return result;
}
std::optional<std::vector<View>> Sections(View data, std::string_view magic)
{
  if (data.size() < 16 || !Magic(data, 0, magic) || Read32(data, 8) != data.size() ||
      Read16(data, 4) != 0xfeff || Read16(data, 6) != 8)
    return {};
  size_t pos = Read16(data, 12);
  if (pos < 16 || pos > data.size())
    return {};
  std::vector<View> result;
  for (size_t i = 0; i < Read16(data, 14); ++i)
  {
    if (data.size() - pos < 8)
      return {};
    const size_t size = Read32(data, pos + 4);
    if (size < 8 || size > data.size() - pos)
      return {};
    result.push_back(data.subspan(pos, size));
    pos += size;
  }
  if (pos != data.size())
    return {};
  return result;
}
std::optional<Bytes> SimplifyLayout(View layout)
{
  const auto sections = Sections(layout, "RLYT");
  if (!sections)
    return {};
  View lyt, textures, materials, root, bg, logo, start, end, group;
  for (View section : *sections)
  {
    if (Magic(section, 0, "lyt1"))
      lyt = section;
    if (Magic(section, 0, "txl1"))
      textures = section;
    if (Magic(section, 0, "mat1"))
      materials = section;
    if (Magic(section, 0, "pan1") && root.empty())
      root = section;
    if (Magic(section, 0, "pas1") && start.empty())
      start = section;
    if (Magic(section, 0, "pae1") && end.empty())
      end = section;
    if (Magic(section, 0, "grp1") && group.empty())
      group = section;
    if (Magic(section, 0, "pic1") && section.size() == 128)
    {
      const auto name = Name(section.subspan(12, 16));
      if (name == "bg")
        bg = section;
      if (name == "Picture_61")
        logo = section;
    }
  }
  if (lyt.empty() || textures.empty() || materials.size() < 12 || root.size() != 76 || bg.empty() ||
      logo.empty() || start.empty() || end.empty() || group.size() != 28)
    return {};
  const size_t count = Read16(materials, 8);
  if (count == 0 || count > 128 || count > (materials.size() - 12) / 4)
    return {};
  Bytes mat(20);
  std::memcpy(mat.data(), "mat1", 4);
  Put16(mat, 8, 2);
  size_t index = 0;
  for (View picture : {bg, logo})
  {
    const size_t id = Read16(picture, 92);
    if (id >= count)
      return {};
    const size_t begin = Read32(materials, 12 + id * 4);
    const size_t finish = id + 1 < count ? Read32(materials, 12 + (id + 1) * 4) : materials.size();
    if (begin < 12 + count * 4 || begin >= finish || finish > materials.size())
      return {};
    Put32(mat, 12 + index++ * 4, static_cast<u32>(mat.size()));
    Append(mat, materials.subspan(begin, finish - begin));
  }
  Put32(mat, 4, static_cast<u32>(mat.size()));
  Bytes background(bg.begin(), bg.end()), title(logo.begin(), logo.end());
  Put16(background, 92, 0);
  Put16(title, 92, 1);
  Bytes result(layout.begin(), layout.begin() + 16);
  for (View section :
       {lyt, textures, View(mat), root, start, View(background), View(title), end, group})
    Append(result, section);
  Put32(result, 8, static_cast<u32>(result.size()));
  Put16(result, 14, 9);
  return result;
}
std::optional<Bytes> SimplifyAnimation(View animation)
{
  const auto sections = Sections(animation, "RLAN");
  if (!sections || sections->size() != 1)
    return {};
  const View pai = sections->front();
  if (pai.size() < 20 || !Magic(pai, 0, "pai1") || Read16(pai, 12) != 0)
    return {};
  const size_t count = Read16(pai, 14), table = Read32(pai, 16);
  if (table < 20 || table > pai.size() || count > (pai.size() - table) / 4)
    return {};
  std::vector<View> keep;
  for (size_t i = 0; i < count; ++i)
  {
    const size_t begin = Read32(pai, table + i * 4);
    const size_t end = i + 1 < count ? Read32(pai, table + (i + 1) * 4) : pai.size();
    if (begin < table + count * 4 || begin > end || end > pai.size() || end - begin < 24)
      return {};
    if (Name(pai.subspan(begin, 20)) == "Picture_61")
      keep.push_back(pai.subspan(begin, end - begin));
  }
  if (keep.size() != 2)
    return {};
  Bytes result(animation.begin(), animation.begin() + 16);
  Append(result, pai.first(20));
  result.resize(16 + 20 + keep.size() * 4);
  for (size_t i = 0; i < keep.size(); ++i)
  {
    Put32(result, 16 + 20 + i * 4, static_cast<u32>(result.size() - 16));
    Append(result, keep[i]);
  }
  Put32(result, 8, static_cast<u32>(result.size()));
  Put32(result, 20, static_cast<u32>(result.size() - 16));
  Put16(result, 30, static_cast<u16>(keep.size()));
  Put32(result, 32, 20);
  return result;
}
}  // namespace

std::optional<std::vector<u8>> PrepareChannelBanner(std::string_view game_id, View banner)
{
  if (!IsBannerSafe(banner))
    return {};
  if (!game_id.starts_with("SSQ"))
    return Bytes(banner.begin(), banner.end());
  const auto outer = Entries(banner.subspan(0x600));
  if (!outer)
    return {};
  const auto icon = std::find_if(outer->begin(), outer->end(),
                                 [](const Entry& entry) { return entry.name == "icon.bin"; });
  if (icon == outer->end())
    return {};
  const auto unpacked = Unpack(banner.subspan(0x600 + icon->offset, icon->size));
  if (!unpacked)
    return {};
  const auto inner = Entries(*unpacked);
  if (!inner)
    return {};
  std::map<std::string, Bytes> replacements;
  for (const auto& file : *inner)
  {
    const View data = View(*unpacked).subspan(file.offset, file.size);
    std::optional<Bytes> replacement;
    if (file.name == "icon.brlyt")
      replacement = SimplifyLayout(data);
    else if (file.name == "icon.brlan")
      replacement = SimplifyAnimation(data);
    else
      continue;
    if (!replacement)
      return {};
    replacements.emplace(file.name, std::move(*replacement));
  }
  if (replacements.size() != 2)
    return {};
  const auto new_icon = Rebuild(*unpacked, replacements);
  if (!new_icon)
    return {};
  const auto archive = Rebuild(banner.subspan(0x600), {{"icon.bin", Wrap(*new_icon)}});
  if (!archive)
    return {};
  Bytes result(banner.begin(), banner.begin() + 0x600);
  Put32(result, 0x4c, static_cast<u32>(new_icon->size()));
  std::fill(result.begin() + 0x5f0, result.end(), 0);
  mbedtls_md5_ret(result.data(), result.size(), result.data() + 0x5f0);
  Append(result, *archive);
  if (!IsBannerSafe(result))
    return {};
  return result;
}
}  // namespace WiiForwarder
