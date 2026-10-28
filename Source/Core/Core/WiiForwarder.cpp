// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiForwarder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mbedtls/md5.h>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "Core/Core.h"
#include "Core/System.h"

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/WiiBanner.h"
#include "Core/WiiMenuBannerHeap.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace WiiForwarder
{
// --- Pending forwarder boot state ---

static std::mutex s_pending_boot_mutex;
static std::string s_pending_boot_path;

void SetPendingForwarderBoot(const std::string& disc_image_path)
{
  std::lock_guard lock(s_pending_boot_mutex);
  s_pending_boot_path = disc_image_path;
}

std::string ConsumePendingForwarderBoot()
{
  std::lock_guard lock(s_pending_boot_mutex);
  std::string path = std::move(s_pending_boot_path);
  s_pending_boot_path.clear();
  return path;
}

bool HasPendingForwarderBoot()
{
  std::lock_guard lock(s_pending_boot_mutex);
  return !s_pending_boot_path.empty();
}

// --- Mapping file I/O ---

std::string GetForwarderMappingFilePath()
{
  return File::GetUserPath(D_WIIROOT_IDX) + "dolphin_forwarders.ini";
}

static Common::IniFile LoadMappingFile()
{
  Common::IniFile ini;
  ini.Load(GetForwarderMappingFilePath());
  return ini;
}

static bool SaveMappingFile(Common::IniFile& ini)
{
  const auto path = GetForwarderMappingFilePath();
  return ini.Save(path + ".tmp") && File::RenameSync(path + ".tmp", path);
}

std::string NormalizePath(const std::string& path)
{
  std::error_code error;
  const std::filesystem::path utf8_path(std::u8string(path.begin(), path.end()));
  auto normalized = std::filesystem::weakly_canonical(utf8_path, error);
  auto bytes = (error ? utf8_path.lexically_normal() : normalized).generic_u8string();
  std::string result(bytes.begin(), bytes.end());
#ifdef _WIN32
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
  return result;
}

size_t AvailableSlots(size_t visible_channels)
{
  // Four pages of 12 cells; the Disc Channel occupies one cell.
  return visible_channels >= 47 ? 0 : 47 - visible_channels;
}

size_t CountVisibleChannels()
{
  size_t count = 0;
  for (const auto* type : {"00010001", "00010002"})
  {
    const auto tree =
        File::ScanDirectoryTree(File::GetUserPath(D_WIIROOT_IDX) + "title/" + type, false);
    for (const auto& child : tree.children)
      if (child.isDirectory && File::Exists(child.physicalName + "/content/title.tmd"))
        ++count;
  }
  return count;
}

bool IsBannerSafe(std::span<const u8> data)
{
  if (data.size() < 0x620 || data.size() > 16 * 1024 * 1024 ||
      std::memcmp(data.data() + 0x40, "IMET", 4) != 0)
    return false;
  const size_t base = 0x600;
  const auto read = [&](size_t offset) -> u32 {
    return (u32(data[offset]) << 24) | (u32(data[offset + 1]) << 16) |
           (u32(data[offset + 2]) << 8) | data[offset + 3];
  };
  if (read(base) != 0x55aa382d)
    return false;
  const size_t root = read(base + 4);
  if (root > data.size() - base - 12)
    return false;
  const size_t node = base + root;
  const size_t count = read(node + 8);
  if (count == 0 || count > 4096 || count > (data.size() - node) / 12)
    return false;
  const size_t strings = node + count * 12;
  bool banner = false, icon = false;
  for (size_t i = 1; i < count; ++i)
  {
    const size_t entry = node + i * 12;
    const size_t name_offset = read(entry) & 0xffffff;
    if (name_offset >= data.size() - strings)
      return false;
    const auto* name = reinterpret_cast<const char*>(data.data() + strings + name_offset);
    const size_t remaining = data.size() - strings - name_offset;
    const auto* end = static_cast<const char*>(std::memchr(name, 0, remaining));
    if (!end)
      return false;
    if (data[entry] == 1)
    {
      if (read(entry + 8) > count || read(entry + 8) <= i)
        return false;
      continue;
    }
    if (data[entry] != 0)
      return false;
    const size_t offset = read(entry + 4), length = read(entry + 8);
    if (offset > data.size() - base || length > data.size() - base - offset || length == 0)
      return false;
    const std::string_view filename(name, end - name);
    if (filename == "banner.bin" || filename == "icon.bin" || filename == "sound.bin")
    {
      const size_t payload = base + offset;
      if (length < 32 || std::memcmp(data.data() + payload, "IMD5", 4) != 0 ||
          read(payload + 4) != length - 32)
        return false;
      std::array<u8, 16> digest{};
      if (mbedtls_md5_ret(data.data() + payload + 32, length - 32, digest.data()) != 0 ||
          !std::equal(digest.begin(), digest.end(), data.begin() + payload + 16))
        return false;
    }
    if (filename == "banner.bin")
      banner = true;
    if (filename == "icon.bin")
      icon = true;
  }
  return banner && icon;
}

static bool OwnsChannel(u64 title, std::string_view directory = "content")
{
  if (static_cast<u32>(title >> 32) != FORWARDER_TITLE_TYPE)
    return false;
  const auto path = Common::GetTitlePath(title, Common::FromWhichRoot::Configured) + "/" +
                    std::string(directory) + "/00000000.app";
  if (File::GetSize(path) > 16 * 1024 * 1024)
    return false;
  std::string content;
  if (!File::ReadFileToString(path, content))
    return false;
  const auto marker = content.rfind("DFWD");
  return marker != std::string::npos && marker + 5 < content.size() && content.back() == 0 &&
         IsBannerSafe(std::span(reinterpret_cast<const u8*>(content.data()), marker));
}

static std::mutex s_storage_mutex;

// --- Title ID generation ---

u64 GenerateForwarderTitleID(const std::string& game_id)
{
  // Hash the game ID to produce a deterministic lower-32 value.
  // Use first 4 bytes of SHA1 XOR'd with our magic to avoid collisions with real channels.
  const auto hash =
      Common::SHA1::CalculateDigest(reinterpret_cast<const u8*>(game_id.data()), game_id.size());
  u32 low32 = 0;
  low32 |= static_cast<u32>(hash[0]) << 24;
  low32 |= static_cast<u32>(hash[1]) << 16;
  low32 |= static_cast<u32>(hash[2]) << 8;
  low32 |= static_cast<u32>(hash[3]);
  low32 ^= FORWARDER_MAGIC;

  return (static_cast<u64>(FORWARDER_TITLE_TYPE) << 32) | low32;
}

bool IsForwarderTitle(u64 title_id)
{
  const auto forwarders = GetInstalledForwarders();
  return forwarders.find(title_id) != forwarders.end() && OwnsChannel(title_id);
}

std::optional<std::string> GetDiscImagePath(u64 forwarder_title_id)
{
  const auto forwarders = GetInstalledForwarders();
  auto it = forwarders.find(forwarder_title_id);
  if (it != forwarders.end())
    return it->second;
  return std::nullopt;
}

bool IsForwarderInstalled(const std::string& disc_image_path)
{
  const auto forwarders = GetInstalledForwarders();
  for (const auto& [tid, path] : forwarders)
  {
    if (NormalizePath(path) == NormalizePath(disc_image_path) && OwnsChannel(tid))
      return true;
  }
  return false;
}

std::map<u64, std::string> GetInstalledForwarders()
{
  std::map<u64, std::string> result;
  Common::IniFile ini = LoadMappingFile();
  const Common::IniFile::Section* section = ini.GetSection("Forwarders");
  if (!section)
    return result;

  for (const auto& [key, value] : section->GetValues())
  {
    u64 title_id = 0;
    if (key.size() == 16 && TryParse(key, &title_id, 16) &&
        static_cast<u32>(title_id >> 32) == FORWARDER_TITLE_TYPE)
      result[title_id] = value;
  }
  return result;
}

// --- Synthetic TMD/Ticket/Content builders ---

// Build a fakesigned TMD blob for a forwarder channel.
static std::vector<u8> BuildForwarderTMD(u64 forwarder_title_id, u64 ios_id,
                                         std::span<const u8> content_data)
{
  const std::size_t tmd_size = sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content);
  std::vector<u8> tmd_bytes(tmd_size, 0);

  auto* header = reinterpret_cast<IOS::ES::TMDHeader*>(tmd_bytes.data());

  // Set signature type to RSA-2048 (0x00010001 big-endian)
  const u32 sig_type_be = Common::swap32(0x00010001u);
  std::memcpy(&header->signature.type, &sig_type_be, sizeof(sig_type_be));
  // Issuer
  std::strncpy(header->signature.issuer, "Root-CA00000001-CP00000004",
               sizeof(header->signature.issuer) - 1);

  // TMD fields
  header->tmd_version = 0;
  header->ios_id = Common::swap64(ios_id);
  header->title_id = Common::swap64(forwarder_title_id);
  header->title_flags = Common::swap32(IOS::ES::TITLE_TYPE_DEFAULT);
  header->group_id = 0;
  header->region = Common::swap16(static_cast<u16>(3));  // Region-free
  header->access_rights = 0;
  header->title_version = Common::swap16(static_cast<u16>(1));
  header->num_contents = Common::swap16(static_cast<u16>(1));
  header->boot_index = Common::swap16(static_cast<u16>(0));

  // Content entry
  auto* content =
      reinterpret_cast<IOS::ES::Content*>(tmd_bytes.data() + sizeof(IOS::ES::TMDHeader));
  content->id = 0;
  content->index = 0;
  content->type = Common::swap16(static_cast<u16>(0x0001));  // Normal content
  content->size = Common::swap64(static_cast<u64>(content_data.size()));

  // SHA1 hash of the content data
  const auto sha1 = Common::SHA1::CalculateDigest(content_data.data(), content_data.size());
  std::copy(sha1.begin(), sha1.end(), content->sha1.begin());

  return tmd_bytes;
}

// Build a fakesigned Ticket blob for a forwarder channel.
static std::vector<u8> BuildForwarderTicket(u64 forwarder_title_id)
{
  std::vector<u8> ticket_bytes(sizeof(IOS::ES::Ticket), 0);
  auto* ticket = reinterpret_cast<IOS::ES::Ticket*>(ticket_bytes.data());

  // Signature type RSA-2048 (0x00010001 big-endian)
  const u32 ticket_sig_type_be = Common::swap32(0x00010001u);
  std::memcpy(&ticket->signature.type, &ticket_sig_type_be, sizeof(ticket_sig_type_be));
  // Issuer
  std::strncpy(ticket->signature.issuer, "Root-CA00000001-XS00000003",
               sizeof(ticket->signature.issuer) - 1);

  ticket->version = 0;
  // Title key: all zeros (we'll encrypt content with zero key)
  std::memset(ticket->title_key, 0, sizeof(ticket->title_key));
  ticket->ticket_id = 0;
  ticket->device_id = 0;
  ticket->title_id = Common::swap64(forwarder_title_id);
  ticket->access_mask = Common::swap16(static_cast<u16>(0xFFFF));
  ticket->ticket_version = 0;
  ticket->common_key_index = 0;

  // Grant access to all contents
  std::memset(ticket->content_access_permissions, 0xFF, sizeof(ticket->content_access_permissions));

  return ticket_bytes;
}

// Extract the full opening.bnr from the disc image. This contains the IMET header
// plus the U8 archive with icon.bin, banner.bin, and sound.bin â€” giving us the
// animated banner, icon, and sound that the Wii Menu displays for channels.
// Missing or malformed banners are rejected by the installer.
static std::vector<u8> ExtractOpeningBnr(const DiscIO::Volume& volume,
                                         const DiscIO::Partition& partition)
{
  const DiscIO::FileSystem* fs = volume.GetFileSystem(partition);
  if (!fs)
    return {};

  std::unique_ptr<DiscIO::FileInfo> file_info = fs->FindFileInfo("opening.bnr");
  if (!file_info || file_info->IsDirectory())
    return {};

  const u64 file_size = file_info->GetSize();
  if (file_size < 0x620 || file_size > 16 * 1024 * 1024)  // Must be at least IMET header size
    return {};

  std::vector<u8> data(file_size);
  const u64 bytes_read =
      DiscIO::ReadFile(volume, partition, file_info.get(), data.data(), file_size);
  if (bytes_read != file_size)
    return {};

  // Verify IMET magic at offset 0x40
  if (data[0x40] != 'I' || data[0x41] != 'M' || data[0x42] != 'E' || data[0x43] != 'T')
    return {};

  return data;
}

static bool WriteBytes(const std::string& path, std::span<const u8> bytes)
{
  if (!File::CreateFullPath(path))
    return false;
  File::IOFile output(path, "wb");
  return output && output.WriteBytes(bytes.data(), bytes.size()) && output.Flush() &&
         output.Close();
}

// Revalidate every owned icon; recover older compatibility conversions from original resources.
// Retained old content also provides recovery if publication is interrupted between renames.
static bool RepairChannelIcons(Common::IniFile& ini)
{
  const size_t heap_size = GetProfileIconHeapSize();
  auto candidates = GetInstalledForwarders();
  std::vector<u64> legacy_ids;
  // These extra IDs only recover quarantine directories created by the older MP9-specific policy.
  for (char region = 'A'; region <= 'Z'; ++region)
  {
    const std::string game_id = std::string("SSQ") + region + "01";
    const u64 id = GenerateForwarderTitleID(game_id);
    legacy_ids.push_back(id);
    candidates.try_emplace(id, "");
  }
  for (const auto& [id, mapped_path] : candidates)
  {
    const auto key = fmt::format("{:016x}", id);
    const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
    const auto active = title + "/content";
    const auto backup = title + "/content.before-icon-format-2";
    const auto stage = title + "/content.icon-fix";
    std::string source;
    for (const auto* directory : {"content", "content.before-icon-format-2",
                                  "content.before-icon-fix", "content.quarantined"})
    {
      if (((std::string_view(directory) == "content.before-icon-fix" ||
            std::string_view(directory) == "content.before-icon-format-2") &&
           !ini.Exists("IconFix", key)) ||
          (std::string_view(directory) == "content.quarantined" && ini.Exists("IconRepaired", key)))
        continue;
      if (OwnsChannel(id, directory))
      {
        source = title + "/" + directory;
        break;
      }
    }
    if (source.empty())
      continue;
    // A genuine active title must never be replaced by a leftover archived forwarder.
    if (source != active && File::Exists(active))
      return false;
    std::string bytes, metadata;
    if (!File::ReadFileToString(source + "/00000000.app", bytes) ||
        !File::ReadFileToString(source + "/title.tmd", metadata))
      return false;
    const auto marker = bytes.rfind("DFWD");
    int format = 0;
    ini.GetOrCreateSection("IconFormat")->Get(key, &format, 0);
    const std::string path = bytes.substr(marker + 4, bytes.size() - marker - 5);
    std::vector<u8> original;
    // Restore the full disc icon when upgrading 1.1.1's deliberately simplified tile.
    if (format < 2)
    {
      auto volume = DiscIO::CreateVolume(path);
      if (volume && volume->GetVolumeType() == DiscIO::Platform::WiiDisc &&
          GenerateForwarderTitleID(volume->GetGameID(volume->GetGamePartition())) == id)
        original = ExtractOpeningBnr(*volume, volume->GetGamePartition());
      if (original.empty() && OwnsChannel(id, "content.before-icon-fix"))
      {
        std::string archived;
        if (File::ReadFileToString(title + "/content.before-icon-fix/00000000.app", archived))
        {
          const auto end = archived.rfind("DFWD");
          if (archived.substr(end + 4) == bytes.substr(marker + 4))
            original.assign(archived.begin(), archived.begin() + end);
        }
      }
    }
    const auto input = original.empty() ?
                           std::span(reinterpret_cast<const u8*>(bytes.data()), marker) :
                           std::span<const u8>(original);
    const auto prepared = PrepareChannelBanner("", input, heap_size);
    if (!prepared)
      return false;
    const IOS::ES::TMDReader old_tmd(std::vector<u8>(metadata.begin(), metadata.end()));
    if (!old_tmd.IsValid() || old_tmd.GetTitleId() != id)
      return false;
    if (!File::Exists(Common::GetTicketFileName(id, Common::FromWhichRoot::Configured)))
      return false;
    const bool unchanged =
        source == active && prepared->size() == marker &&
        std::equal(prepared->begin(), prepared->end(), reinterpret_cast<const u8*>(bytes.data()));
    if (!unchanged)
    {
      ini.GetOrCreateSection("IconFix")->Set(key, true);
      if (!SaveMappingFile(ini))
        return false;
      auto content = *prepared;
      content.insert(content.end(), bytes.begin() + marker, bytes.end());
      const auto tmd = BuildForwarderTMD(id, old_tmd.GetIOSId(), content);
      // Only remove our partial stage after ownership of the original has been established.
      if (File::Exists(stage) && !File::DeleteDirRecursively(stage))
        return false;
      if (!WriteBytes(stage + "/00000000.app", content) || !WriteBytes(stage + "/title.tmd", tmd))
        return false;
      if (source == active && (File::Exists(backup) || !File::RenameSync(active, backup)))
        return false;
      if (!File::RenameSync(stage, active))
        return false;
      INFO_LOG_FMT(CORE, "Updated channel icon compatibility: {}", path);
    }
    ini.GetOrCreateSection("Forwarders")->Set(key, path);
    ini.DeleteKey("Excluded", key);
    ini.GetOrCreateSection("IconRepaired")->Set(key, true);
    // An offline 1.1.1 tile can remain usable in its old form. Retry its full-art restoration
    // when the disc/backup returns instead of marking that migration complete prematurely.
    if (format >= 2 || !original.empty() ||
        std::find(legacy_ids.begin(), legacy_ids.end(), id) == legacy_ids.end())
      ini.GetOrCreateSection("IconFormat")->Set(key, 2);
    ini.DeleteKey("IconFix", key);
  }
  return true;
}

// --- Install/Uninstall ---

bool InstallForwarder(const std::string& path, bool silent)
{
  auto volume = DiscIO::CreateVolume(path);
  if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
  {
    if (!silent)
      PanicAlertFmtT("Only readable Wii disc images can be added to the Wii Menu.");
    return false;
  }
  const auto partition = volume->GetGamePartition();
  const auto& tmd = volume->GetTMD(partition);
  return InstallChannel({volume->GetGameID(partition), path, ExtractOpeningBnr(*volume, partition),
                         tmd.IsValid() ? tmd.GetIOSId() : 0x0000000100000038},
                        silent);
}

bool InstallChannel(const ChannelData& source, bool silent)
{
  const auto& disc_image_path = source.path;
  const auto& game_id = source.game_id;
  std::lock_guard lock(s_storage_mutex);
  const auto fail = [&](const std::string& message) {
    ERROR_LOG_FMT(CORE, "WiiForwarder: {}", message);
    if (!silent)
      PanicAlertFmtT("{0}", message);
    return false;
  };
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return fail("Stop emulation before changing Wii Menu channels.");
  if (!File::Exists(File::GetUserPath(D_WIIROOT_IDX) + "title/00000001/00000002/content/title.tmd"))
    return fail("Install the Wii Menu first by opening the Wii Menu tile in your library.");
  if (game_id.size() != 6)
    return fail("The disc image has an invalid game ID.");
  const u64 title = GenerateForwarderTitleID(game_id);
  const auto key = fmt::format("{:016x}", title);
  auto ini = LoadMappingFile();
  if (silent && ini.Exists("Excluded", key))
    return false;
  const auto mapping = GetInstalledForwarders();
  if (const auto old = mapping.find(title); old != mapping.end() && OwnsChannel(title))
  {
    // Same game at another path: deduplicate while the original is online; relink if it moved.
    if (!File::Exists(old->second))
    {
      ini.GetOrCreateSection("Forwarders")->Set(key, NormalizePath(disc_image_path));
      if (!SaveMappingFile(ini))
        return fail("Could not save the relocated game's path.");
    }
    return true;
  }
  const auto root = Common::FromWhichRoot::Configured;
  const auto title_path = Common::GetTitlePath(title, root);
  const auto ticket = Common::GetTicketFileName(title, root);
  if (File::Exists(title_path + "/content") || File::Exists(ticket) ||
      (File::Exists(title_path) && !ini.Exists("Excluded", key)))
    return fail("This title ID is already occupied. Existing channels will not be overwritten.");
  if (AvailableSlots(CountVisibleChannels()) == 0)
    return fail(
        "The Wii Menu is full (48 tiles including the Disc Channel). Remove a channel first.");
  const auto prepared = PrepareChannelBanner(game_id, source.banner, GetProfileIconHeapSize());
  if (!prepared)
    return fail("This game's banner cannot be safely prepared for the Wii Menu. Launch it from the "
                "library.");
  auto content = *prepared;
  const std::string marker = "DFWD" + disc_image_path;
  content.insert(content.end(), marker.begin(), marker.end());
  content.push_back(0);
  const auto ios = source.ios;
  const auto tmd = BuildForwarderTMD(title, ios, content);
  const auto ticket_data = BuildForwarderTicket(title);
  const auto stage = title_path + ".installing";
  if (File::Exists(stage))
    return fail(
        "An interrupted channel install needs recovery. Use Tools > Repair Wii Menu Channels.");
  // Journal first, so a partial stage is recoverable after a failed write or process exit.
  ini.GetOrCreateSection("Pending")->Set(key, NormalizePath(disc_image_path));
  if (!SaveMappingFile(ini))
    return fail("Could not save the channel mapping; no channel was installed.");
  if (!WriteBytes(stage + "/content/00000000.app", content) ||
      !WriteBytes(stage + "/content/title.tmd", tmd) ||
      !WriteBytes(stage + "/ticket.tik", ticket_data))
    return fail("Could not stage the channel. Check space and permissions, then repair channels.");
  if (!File::CreateFullPath(ticket) || !File::CreateFullPath(title_path + "/data/") ||
      !File::RenameSync(stage + "/ticket.tik", ticket) ||
      !File::RenameSync(stage + "/content", title_path + "/content"))
    return fail("Channel publication failed. Use Tools > Repair Wii Menu Channels to recover.");
  File::DeleteDirRecursively(stage);
  ini.GetOrCreateSection("Forwarders")->Set(key, NormalizePath(disc_image_path));
  ini.GetOrCreateSection("IconFormat")->Set(key, 2);
  ini.DeleteKey("Pending", key);
  ini.DeleteKey("Excluded", key);
  if (!SaveMappingFile(ini))
    return fail("The channel was written but its mapping could not be finalized. Repair channels "
                "to finish.");
  return true;
}

bool RepairChannels()
{
  std::lock_guard lock(s_storage_mutex);
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return false;
  auto ini = LoadMappingFile();
  // Finish an interrupted explicit removal before considering another sync.
  if (const auto* removals = ini.GetSection("Removing"))
  {
    const auto entries = removals->GetValues();
    for (const auto& [key, ignored] : entries)
    {
      u64 id = 0;
      if (key.size() != 16 || !TryParse(key, &id, 16) ||
          static_cast<u32>(id >> 32) != FORWARDER_TITLE_TYPE)
        return false;
      const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
      const auto ticket = Common::GetTicketFileName(id, Common::FromWhichRoot::Configured);
      const auto removed = title + "/content.removing";
      if (OwnsChannel(id) && !File::RenameSync(title + "/content", removed))
        return false;
      if (File::Exists(ticket) && !File::Delete(ticket))
        return false;
      if (File::Exists(removed) && !File::DeleteDirRecursively(removed))
        return false;
      ini.DeleteKey("Forwarders", key);
      ini.GetOrCreateSection("Excluded")->Set(key, true);
      ini.DeleteKey("IconFix", key);
      ini.GetOrCreateSection("IconRepaired")->Set(key, true);
      ini.DeleteKey("Removing", key);
    }
  }
  const auto* pending = ini.GetSection("Pending");
  if (!pending)
    return RepairChannelIcons(ini) && SaveMappingFile(ini);
  const auto values = pending->GetValues();
  for (const auto& [key, path] : values)
  {
    u64 id = 0;
    if (key.size() != 16 || !TryParse(key, &id, 16) ||
        static_cast<u32>(id >> 32) != FORWARDER_TITLE_TYPE)
      return false;
    const auto title = Common::GetTitlePath(id, Common::FromWhichRoot::Configured);
    const auto ticket = Common::GetTicketFileName(id, Common::FromWhichRoot::Configured);
    if (OwnsChannel(id) && File::Exists(ticket))
      ini.GetOrCreateSection("Forwarders")->Set(key, path);
    else
    {
      // Only incomplete titles with our marker can be removed, never unrelated content.
      if (File::Exists(title + "/content"))
        return false;
      if (File::Exists(ticket) && !File::Delete(ticket))
        return false;
      if (File::Exists(title + ".installing") && !File::DeleteDirRecursively(title + ".installing"))
        return false;
    }
    ini.DeleteKey("Pending", key);
    if (File::Exists(title + ".installing") && !File::DeleteDirRecursively(title + ".installing"))
      return false;
  }
  return RepairChannelIcons(ini) && SaveMappingFile(ini);
}

bool UninstallForwarder(u64 title)
{
  std::lock_guard lock(s_storage_mutex);
  if (!Core::IsUninitialized(Core::System::GetInstance()) || !IsForwarderTitle(title) ||
      !OwnsChannel(title))
    return false;
  auto ini = LoadMappingFile();
  const auto key = fmt::format("{:016x}", title);
  const auto path = Common::GetTitlePath(title, Common::FromWhichRoot::Configured);
  const auto ticket = Common::GetTicketFileName(title, Common::FromWhichRoot::Configured);
  // Move content aside before committing the mapping. Preserve the data directory (saves).
  const auto content = path + "/content";
  const auto removed = path + "/content.removing";
  if (File::Exists(removed))
    return false;
  ini.GetOrCreateSection("Removing")->Set(key, true);
  if (!SaveMappingFile(ini) || !File::RenameSync(content, removed))
    return false;
  ini.DeleteKey("Forwarders", key);
  ini.GetOrCreateSection("Excluded")->Set(key, true);
  ini.DeleteKey("IconFix", key);
  ini.GetOrCreateSection("IconRepaired")->Set(key, true);
  if (!SaveMappingFile(ini))
  {
    File::RenameSync(removed, content);
    return false;
  }
  const bool ticket_ok = !File::Exists(ticket) || File::Delete(ticket);
  const bool content_ok = File::DeleteDirRecursively(removed);
  if (!ticket_ok || !content_ok)
    return false;
  ini.DeleteKey("Removing", key);
  return SaveMappingFile(ini);
}
}  // namespace WiiForwarder
