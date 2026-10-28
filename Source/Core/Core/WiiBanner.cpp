// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiBanner.h"

#include <algorithm>
#include <cstring>
#include <string>

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
bool Magic(View data, size_t offset, std::string_view magic)
{
  return offset <= data.size() && magic.size() <= data.size() - offset &&
         std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
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

struct Usage
{
  size_t panes = 0, materials = 0, bindings = 0, keys = 0, palettes = 0;
};

bool CheckLayout(View layout, Usage& usage)
{
  const auto sections = Sections(layout, "RLYT");
  if (!sections)
    return false;
  size_t depth = 0, textures = 0, materials = 0;
  for (View section : *sections)
  {
    if (Magic(section, 0, "txl1") || Magic(section, 0, "fnl1"))
    {
      if (section.size() < 12 || Read16(section, 8) > (section.size() - 12) / 8)
        return false;
      if (Magic(section, 0, "txl1"))
        textures = Read16(section, 8);
      const size_t count = Read16(section, 8);
      for (size_t i = 0; i < count; ++i)
      {
        const size_t offset = Read32(section, 12 + i * 8);
        if (offset < count * 8 || offset >= section.size() - 12 ||
            std::find(section.begin() + 12 + offset, section.end(), 0) == section.end())
          return false;
      }
    }
    if (Magic(section, 0, "mat1"))
    {
      if (section.size() < 12)
        return false;
      materials = Read16(section, 8);
      if (materials > 256 || materials > (section.size() - 12) / 4)
        return false;
      usage.materials += materials;
      for (size_t i = 0; i < materials; ++i)
      {
        const size_t start = Read32(section, 12 + i * 4);
        const size_t end = i + 1 < materials ? Read32(section, 12 + (i + 1) * 4) : section.size();
        if (start < 12 + materials * 4 || start > end || end > section.size() || end - start < 64)
          return false;
        const size_t maps = Read32(section, start + 60) & 15;
        if (maps > 8 || maps > (end - start - 64) / 4)
          return false;
        for (size_t map = 0; map < maps; ++map)
          if (Read16(section, start + 64 + map * 4) >= textures)
            return false;
      }
    }
  }
  for (View section : *sections)
  {
    if (Magic(section, 0, "pan1") || Magic(section, 0, "pic1") || Magic(section, 0, "txt1") ||
        Magic(section, 0, "wnd1") || Magic(section, 0, "bnd1"))
    {
      if (section.size() < 76 || ++usage.panes > 512)
        return false;
      if (Magic(section, 0, "pic1"))
      {
        if (section.size() < 96 || Read16(section, 92) >= materials ||
            size_t(section[94]) > (section.size() - 96) / 32)
          return false;
      }
      if (Magic(section, 0, "txt1"))
      {
        if (section.size() < 116 || Read16(section, 80) >= materials)
          return false;
        const size_t offset = Read32(section, 88), length = Read16(section, 78);
        if (offset > section.size() || length > section.size() - offset)
          return false;
      }
      if (Magic(section, 0, "wnd1"))
      {
        if (section.size() < 104)
          return false;
        const size_t count = section[92], table = Read32(section, 100);
        if (table > section.size() || count > (section.size() - table) / 4)
          return false;
        for (size_t i = 0; i < count; ++i)
        {
          const size_t offset = Read32(section, table + i * 4);
          if (offset > section.size() - 3 || Read16(section, offset) >= materials)
            return false;
        }
      }
    }
    if (Magic(section, 0, "pas1"))
    {
      if (++depth > 64)
        return false;
    }
    if (Magic(section, 0, "pae1"))
    {
      if (!depth)
        return false;
      --depth;
    }
    if (Magic(section, 0, "grp1") &&
        (section.size() < 28 || size_t(Read16(section, 24)) > (section.size() - 28) / 16))
      return false;
  }
  return depth == 0 && usage.panes != 0;
}

bool CheckAnimation(View animation, Usage& usage)
{
  const auto sections = Sections(animation, "RLAN");
  if (!sections)
    return false;
  for (View pai : *sections)
  {
    if (!Magic(pai, 0, "pai1"))
      continue;
    if (pai.size() < 20)
      return false;
    const size_t count = Read16(pai, 14), table = Read32(pai, 16), files = Read16(pai, 12);
    if (table < 20 || table > pai.size() || count > (pai.size() - table) / 4 ||
        files > (pai.size() - 20) / 4 || files > 64)
      return false;
    usage.bindings += count;
    usage.palettes += files;
    if (usage.bindings > 512)
      return false;
    for (size_t i = 0; i < files; ++i)
    {
      const size_t offset = Read32(pai, 20 + i * 4);
      if (offset >= pai.size() - 20 ||
          std::find(pai.begin() + 20 + offset, pai.end(), 0) == pai.end())
        return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
      const size_t offset = Read32(pai, table + i * 4);
      if (offset < table + count * 4 || offset > pai.size() || pai.size() - offset < 24)
        return false;
      const size_t tags = pai[offset + 20];
      if (tags > (pai.size() - offset - 24) / 4)
        return false;
      for (size_t tag = 0; tag < tags; ++tag)
      {
        const size_t relative = Read32(pai, offset + 24 + tag * 4);
        if (relative > pai.size() - offset || pai.size() - offset - relative < 8)
          return false;
        const size_t info = offset + relative, tracks = pai[info + 4];
        if (tracks > (pai.size() - info - 8) / 4)
          return false;
        for (size_t track = 0; track < tracks; ++track)
        {
          const size_t key_offset = Read32(pai, info + 8 + track * 4);
          if (key_offset > pai.size() - info || pai.size() - info - key_offset < 12)
            return false;
          const size_t key = info + key_offset, length = Read16(pai, key + 4);
          const size_t data = Read32(pai, key + 8), stride = pai[key + 2] == 1 ? 8 : 12;
          if ((pai[key + 2] != 1 && pai[key + 2] != 2) || data < 12 || data > pai.size() - key ||
              length > (pai.size() - key - data) / stride)
            return false;
          usage.keys += length;
          if (usage.keys > 65536)
            return false;
        }
      }
    }
  }
  return true;
}

bool CheckIconBudget(View archive, const std::vector<Entry>& entries, size_t heap_size)
{
  if (heap_size != STOCK_ICON_HEAP && heap_size != EXPANDED_ICON_HEAP)
    return false;
  Usage usage;
  bool layout = false;
  for (const auto& file : entries)
  {
    const auto data = archive.subspan(file.offset, file.size);
    if (file.name.ends_with(".brlyt"))
    {
      if (!CheckLayout(data, usage))
        return false;
      layout |= file.name == "icon.brlyt";
    }
    if (file.name.ends_with(".brlan") && !CheckAnimation(data, usage))
      return false;
  }
  // Conservative admission estimate, not an assertion that arbitrary guest assets are crash-proof.
  // Leave headroom for allocator bookkeeping and menu-owned layout objects in the fixed heap.
  const size_t working = 0x2000 + usage.panes * 0x180 + usage.materials * 0x180 +
                         usage.bindings * 0x100 + usage.keys * 8 + usage.palettes * 0x200;
  return layout && working <= heap_size * 7 / 8;
}
}  // namespace

std::optional<std::vector<u8>> PrepareChannelBanner(std::string_view, View banner, size_t heap_size)
{
  if (!IsBannerSafe(banner))
    return {};
  const auto outer = Entries(banner.subspan(0x600));
  if (!outer)
    return {};
  const auto icon = std::find_if(outer->begin(), outer->end(),
                                 [](const Entry& entry) { return entry.name == "icon.bin"; });
  if (icon == outer->end())
    return {};
  const auto unpacked = Unpack(banner.subspan(0x600 + icon->offset, icon->size));
  if (!unpacked || unpacked->size() > 0x20000 || Read32(banner, 0x4c) != unpacked->size())
    return {};
  const auto inner = Entries(*unpacked);
  if (!inner || !CheckIconBudget(*unpacked, *inner, heap_size))
    return {};
  return Bytes(banner.begin(), banner.end());
}
}  // namespace WiiForwarder
