#include "libc_shim/vulkan_etc1_sky_transcoder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace libc_shim {
namespace {

constexpr uint32_t kKtx1LittleEndian = 0x04030201;
constexpr uint32_t kGlEtc1Rgb8Oes = 0x00008d64;
constexpr uint32_t kGlCompressedRgbS3tcDxt1 = 0x000083f0;
constexpr uint32_t kGlRgb = 0x00001907;
constexpr size_t kMaxSkyTextureSize = 16U * 1024U * 1024U;
constexpr std::array<uint8_t, 12> kKtx1Identifier = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a,
};

// On-disk cache entry layout. The payload follows a fixed 64-byte header that
// records the identity of the source file, so a stale entry cannot be served
// for a different texture that happens to share a name.
constexpr std::array<char, 8> kCacheMagic = {'M', 'T', 'S', 'K', 'Y', '1',
                                             '\0', '\0'};
constexpr uint32_t kCacheVersion = 1;
constexpr size_t kCacheHeaderBytes = 64;
constexpr size_t kCacheOffsetMagic = 0;
constexpr size_t kCacheOffsetVersion = 8;
constexpr size_t kCacheOffsetPayloadBytes = 12;
constexpr size_t kCacheOffsetSourceBytes = 16;
constexpr size_t kCacheOffsetDevice = 24;
constexpr size_t kCacheOffsetInode = 32;
constexpr size_t kCacheOffsetMtimeSeconds = 40;
constexpr size_t kCacheOffsetMtimeNanoseconds = 48;
constexpr size_t kCacheOffsetCrc = 56;

struct SourceIdentity {
  uint64_t device = 0;
  uint64_t inode = 0;
  uint64_t size = 0;
  int64_t mtime_seconds = 0;
  int64_t mtime_nanoseconds = 0;
};

void StoreLe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8);
  bytes[2] = static_cast<uint8_t>(value >> 16);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

void StoreLe64(uint8_t* bytes, uint64_t value) {
  for (size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<uint8_t>(value >> (index * 8));
  }
}

uint32_t LoadLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t LoadLe64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

// CRC-32 over the stored bytes. The rename that publishes an entry already
// rules out a partial write; this rules out a damaged one, which a disk cache
// read back once per launch is exactly the thing to be suspicious about.
uint32_t Crc32(const uint8_t* data, size_t bytes) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < bytes; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (~(crc & 1U) + 1U));
    }
  }
  return ~crc;
}

std::string CacheDirectory() {
  const char* directory = std::getenv("MOCKTAIL_SKY_TRANSCODE_CACHE_DIR");
  if (directory == nullptr || directory[0] == '\0') {
    return {};
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return {};
  }
  return std::string(directory);
}

std::string CacheFileName(const std::string_view path,
                          const SourceIdentity& identity) {
  // Only the file name is kept: the identity below already says which file this
  // is, and two payloads can hold different textures under one name.
  const std::size_t separator = path.find_last_of('/');
  const std::string_view name = separator == std::string_view::npos
                                    ? path
                                    : path.substr(separator + 1);
  std::string key;
  key.reserve(name.size() + 64);
  for (const char character : name) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '.' || character == '-' || character == '_';
    key.push_back(safe ? character : '_');
  }
  key.push_back('-');
  char digits[33] = {};
  std::snprintf(digits, sizeof(digits), "%016llx%016llx",
                static_cast<unsigned long long>(identity.device),
                static_cast<unsigned long long>(identity.inode));
  key.append(digits);
  key.push_back('-');
  std::snprintf(digits, sizeof(digits), "%016llx%08x%08llx",
                static_cast<unsigned long long>(identity.size),
                static_cast<unsigned>(identity.mtime_seconds),
                static_cast<unsigned long long>(identity.mtime_nanoseconds));
  key.append(digits);
  key.append(".bc1");
  return key;
}

bool ReadSourceIdentity(const char* path, SourceIdentity* identity) {
  if (path == nullptr || identity == nullptr) {
    return false;
  }
  struct stat metadata = {};
  if (::stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0) {
    return false;
  }
  identity->device = static_cast<uint64_t>(metadata.st_dev);
  identity->inode = static_cast<uint64_t>(metadata.st_ino);
  identity->size = static_cast<uint64_t>(metadata.st_size);
  identity->mtime_seconds = static_cast<int64_t>(metadata.st_mtim.tv_sec);
  identity->mtime_nanoseconds =
      static_cast<int64_t>(metadata.st_mtim.tv_nsec);
  return true;
}

bool LoadCachedTranscode(const std::string& file,
                         const SourceIdentity& identity,
                         std::vector<unsigned char>* data) {
  const int descriptor =
      open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  auto release = [&]() { ::close(descriptor); };
  std::array<uint8_t, kCacheHeaderBytes> header{};
  size_t header_bytes = 0;
  while (header_bytes < header.size()) {
    const ssize_t received = read(descriptor, header.data() + header_bytes,
                                  header.size() - header_bytes);
    if (received == 0) {
      release();
      return false;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      release();
      return false;
    }
    header_bytes += static_cast<size_t>(received);
  }
  if (std::memcmp(header.data() + kCacheOffsetMagic, kCacheMagic.data(),
                  kCacheMagic.size()) != 0 ||
      LoadLe32(header.data() + kCacheOffsetVersion) != kCacheVersion ||
      LoadLe64(header.data() + kCacheOffsetSourceBytes) != identity.size ||
      LoadLe64(header.data() + kCacheOffsetDevice) != identity.device ||
      LoadLe64(header.data() + kCacheOffsetInode) != identity.inode ||
      static_cast<int64_t>(LoadLe64(header.data() + kCacheOffsetMtimeSeconds)) !=
          identity.mtime_seconds ||
      static_cast<int64_t>(LoadLe64(header.data() +
                                    kCacheOffsetMtimeNanoseconds)) !=
          identity.mtime_nanoseconds) {
    release();
    return false;
  }
  const uint32_t payload_bytes = LoadLe32(header.data() + kCacheOffsetPayloadBytes);
  const uint32_t expected_crc = LoadLe32(header.data() + kCacheOffsetCrc);
  // The transcode rewrites the file in place, so the entry has to be exactly
  // the size of the texture it came from.
  if (payload_bytes != identity.size || payload_bytes > kMaxSkyTextureSize) {
    release();
    return false;
  }
  std::vector<unsigned char> payload(payload_bytes);
  size_t read_bytes = 0;
  while (read_bytes < payload.size()) {
    const ssize_t received = read(descriptor, payload.data() + read_bytes,
                                  payload.size() - read_bytes);
    if (received == 0) {
      release();
      return false;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      release();
      return false;
    }
    read_bytes += static_cast<size_t>(received);
  }
  // Nothing may follow the payload: a longer file is a different entry.
  uint8_t trailing = 0;
  const ssize_t extra = read(descriptor, &trailing, 1);
  release();
  if (extra != 0 ||
      Crc32(payload.data(), payload.size()) != expected_crc) {
    return false;
  }
  *data = std::move(payload);
  return true;
}

// An entry names one file identity, so every payload leaves the previous
// payload's entries behind. Nothing ever reads those again, so the directory is
// trimmed back to a few payloads' worth. Twelve textures on a cold launch make
// this a dozen scans of a directory that stays under the cap.
constexpr std::size_t kMaximumCacheEntries = 64;

void PruneStaleEntries(const std::filesystem::path& directory) {
  std::error_code error;
  std::vector<std::pair<std::int64_t, std::filesystem::path>> entries;
  for (const auto& entry : std::filesystem::directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied,
           error)) {
    if (error || !entry.is_regular_file(error)) {
      continue;
    }
    const auto written = entry.last_write_time().time_since_epoch().count();
    entries.emplace_back(static_cast<std::int64_t>(written), entry.path());
  }
  if (entries.size() <= kMaximumCacheEntries) {
    return;
  }
  std::sort(entries.begin(), entries.end());
  const std::size_t excess = entries.size() - kMaximumCacheEntries;
  for (std::size_t index = 0; index < excess; ++index) {
    std::error_code remove_error;
    std::filesystem::remove(entries[index].second, remove_error);
  }
}

bool StoreCachedTranscode(const std::string& file,
                          const SourceIdentity& identity,
                          const std::vector<unsigned char>& data) {
  if (data.size() > kMaxSkyTextureSize) {
    return false;
  }
  static std::atomic<uint64_t> s_generation{0};
  const std::string temporary =
      file + ".tmp-" + std::to_string(getpid()) + "-" +
      std::to_string(s_generation.fetch_add(1, std::memory_order_relaxed));
  const int descriptor = open(temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                  O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return false;
  }
  auto discard = [&]() {
    close(descriptor);
    unlink(temporary.c_str());
  };
  std::array<uint8_t, kCacheHeaderBytes> header{};
  std::memcpy(header.data() + kCacheOffsetMagic, kCacheMagic.data(),
              kCacheMagic.size());
  StoreLe32(header.data() + kCacheOffsetVersion, kCacheVersion);
  StoreLe32(header.data() + kCacheOffsetPayloadBytes,
            static_cast<uint32_t>(data.size()));
  StoreLe64(header.data() + kCacheOffsetSourceBytes, identity.size);
  StoreLe64(header.data() + kCacheOffsetDevice, identity.device);
  StoreLe64(header.data() + kCacheOffsetInode, identity.inode);
  StoreLe64(header.data() + kCacheOffsetMtimeSeconds,
            static_cast<uint64_t>(identity.mtime_seconds));
  StoreLe64(header.data() + kCacheOffsetMtimeNanoseconds,
            static_cast<uint64_t>(identity.mtime_nanoseconds));
  StoreLe32(header.data() + kCacheOffsetCrc, Crc32(data.data(), data.size()));

  auto write_all = [&](const uint8_t* bytes, size_t count) {
    size_t offset = 0;
    while (offset < count) {
      const ssize_t written =
          write(descriptor, bytes + offset, count - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      offset += static_cast<size_t>(written);
    }
    return true;
  };
  if (!write_all(header.data(), header.size()) ||
      !write_all(data.data(), data.size())) {
    discard();
    return false;
  }
  if (close(descriptor) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  if (rename(temporary.c_str(), file.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  PruneStaleEntries(std::filesystem::path(file).parent_path());
  return true;
}

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

uint8_t ClampByte(int value) {
  return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

Rgb ApplyModifier(const uint8_t color[3], int modifier) {
  return {ClampByte(color[0] + modifier), ClampByte(color[1] + modifier),
          ClampByte(color[2] + modifier)};
}

int SignExtend3(uint8_t value) {
  return (value & 4) != 0 ? static_cast<int>(value) - 8 : value;
}

uint8_t Expand4(uint8_t value) {
  return static_cast<uint8_t>((value << 4) | value);
}

uint8_t Expand5(int value) {
  return static_cast<uint8_t>((value << 3) | (value >> 2));
}

// ETC decoder adapted from AssetStudio's Texture2DDecoderNative.
// MIT License. Copyright (c) 2016 Radu and (c) 2016-2020 Perfare.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to inclusion of this notice. THE SOFTWARE IS
// PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
void DecodeEtc1RgbBlock(const uint8_t *data, std::array<Rgb, 16> *output) {
  constexpr std::array<uint8_t, 16> kWriteOrder = {
      0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15,
  };
  constexpr uint8_t kModifier[8][2] = {
      {2, 8},   {5, 17},  {9, 29},   {13, 42},
      {18, 60}, {24, 80}, {33, 106}, {47, 183},
  };
  constexpr uint8_t kSubblock[2][16] = {
      {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
      {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
  };

  uint8_t color[2][3] = {};
  if ((data[3] & 2) != 0) {
    for (size_t channel = 0; channel < 3; ++channel) {
      const int base = data[channel] >> 3;
      color[0][channel] = Expand5(base);
      color[1][channel] = Expand5(base + SignExtend3(data[channel] & 7));
    }
  } else {
    for (size_t channel = 0; channel < 3; ++channel) {
      color[0][channel] = Expand4(data[channel] >> 4);
      color[1][channel] = Expand4(data[channel] & 15);
    }
  }

  uint16_t low_indices = static_cast<uint16_t>((data[6] << 8) | data[7]);
  uint16_t high_indices = static_cast<uint16_t>((data[4] << 8) | data[5]);
  const uint8_t code[2] = {static_cast<uint8_t>(data[3] >> 5),
                           static_cast<uint8_t>((data[3] >> 2) & 7)};
  const uint8_t *subblock = kSubblock[data[3] & 1];
  for (size_t pixel = 0; pixel < 16; ++pixel) {
    const uint8_t side = subblock[pixel];
    const uint8_t modifier = kModifier[code[side]][low_indices & 1];
    (*output)[kWriteOrder[pixel]] = ApplyModifier(
        color[side], (high_indices & 1) != 0 ? -modifier : modifier);
    low_indices >>= 1;
    high_indices >>= 1;
  }
}

uint16_t PackRgb565(const Rgb &color) {
  return static_cast<uint16_t>(((color.r >> 3) << 11) | ((color.g >> 2) << 5) |
                               (color.b >> 3));
}

Rgb UnpackRgb565(uint16_t color) {
  const uint8_t r = static_cast<uint8_t>((color >> 11) & 31);
  const uint8_t g = static_cast<uint8_t>((color >> 5) & 63);
  const uint8_t b = static_cast<uint8_t>(color & 31);
  return {static_cast<uint8_t>((r << 3) | (r >> 2)),
          static_cast<uint8_t>((g << 2) | (g >> 4)),
          static_cast<uint8_t>((b << 3) | (b >> 2))};
}

uint32_t ColorDistance(const Rgb &lhs, const Rgb &rhs) {
  const int dr = lhs.r - rhs.r;
  const int dg = lhs.g - rhs.g;
  const int db = lhs.b - rhs.b;
  return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

uint32_t ReadLe32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

void WriteLe32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8);
  bytes[2] = static_cast<uint8_t>(value >> 16);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

bool IsBundledSkyTexture(const char *path) {
  // Also matches the asset overlay's copy of the folder.
  constexpr char kSkyDirectory[] = "/android/textures/sky/";
  if (path == nullptr || std::strstr(path, kSkyDirectory) == nullptr) {
    return false;
  }
  const size_t length = std::strlen(path);
  return length >= 4 && std::memcmp(path + length - 4, ".tex", 4) == 0;
}

bool TranscodeKtx1Etc1(std::vector<uint8_t> *file) {
  constexpr size_t kHeaderSize = 64;
  if (file->size() < kHeaderSize ||
      std::memcmp(file->data(), kKtx1Identifier.data(),
                  kKtx1Identifier.size()) != 0 ||
      ReadLe32(file->data() + 12) != kKtx1LittleEndian ||
      ReadLe32(file->data() + 16) != 0 || ReadLe32(file->data() + 20) != 1 ||
      ReadLe32(file->data() + 24) != 0 ||
      ReadLe32(file->data() + 28) != kGlEtc1Rgb8Oes ||
      ReadLe32(file->data() + 32) != kGlRgb) {
    return false;
  }

  const uint32_t width = ReadLe32(file->data() + 36);
  const uint32_t height = ReadLe32(file->data() + 40);
  const uint32_t depth = ReadLe32(file->data() + 44);
  const uint32_t array_elements = ReadLe32(file->data() + 48);
  const uint32_t faces = ReadLe32(file->data() + 52);
  const uint32_t mip_levels = ReadLe32(file->data() + 56);
  const uint32_t key_value_bytes = ReadLe32(file->data() + 60);
  if (width == 0 || height == 0 || depth != 0 || array_elements != 0 ||
      faces != 1 || mip_levels == 0 || mip_levels > 32 ||
      key_value_bytes > file->size() - kHeaderSize) {
    return false;
  }

  size_t offset = kHeaderSize + key_value_bytes;
  uint32_t mip_width = width;
  uint32_t mip_height = height;
  for (uint32_t level = 0; level < mip_levels; ++level) {
    if (offset > file->size() || file->size() - offset < 4) {
      return false;
    }
    const uint32_t image_size = ReadLe32(file->data() + offset);
    offset += 4;
    const uint64_t block_count = static_cast<uint64_t>((mip_width + 3) / 4) *
                                 static_cast<uint64_t>((mip_height + 3) / 4);
    const uint64_t expected_size = block_count * 8;
    if (image_size != expected_size || image_size > file->size() - offset) {
      return false;
    }
    for (size_t block = 0; block < image_size / 8; ++block) {
      std::array<uint8_t, 8> converted{};
      ConvertEtc1RgbBlockToBc1(file->data() + offset + block * 8,
                               converted.data());
      std::memcpy(file->data() + offset + block * 8, converted.data(),
                  converted.size());
    }
    offset += image_size;
    const size_t padding = (4 - image_size % 4) % 4;
    if (padding > file->size() - offset) {
      return false;
    }
    offset += padding;
    mip_width = std::max(1U, mip_width / 2);
    mip_height = std::max(1U, mip_height / 2);
  }

  WriteLe32(file->data() + 28, kGlCompressedRgbS3tcDxt1);
  return true;
}

} // namespace

void ConvertEtc1RgbBlockToBc1(const uint8_t *etc1, uint8_t *bc1) {
  std::array<Rgb, 16> pixels{};
  DecodeEtc1RgbBlock(etc1, &pixels);

  Rgb minimum = pixels[0];
  Rgb maximum = pixels[0];
  for (const Rgb &pixel : pixels) {
    minimum.r = std::min(minimum.r, pixel.r);
    minimum.g = std::min(minimum.g, pixel.g);
    minimum.b = std::min(minimum.b, pixel.b);
    maximum.r = std::max(maximum.r, pixel.r);
    maximum.g = std::max(maximum.g, pixel.g);
    maximum.b = std::max(maximum.b, pixel.b);
  }
  const uint8_t inset_r = static_cast<uint8_t>((maximum.r - minimum.r) / 16);
  const uint8_t inset_g = static_cast<uint8_t>((maximum.g - minimum.g) / 16);
  const uint8_t inset_b = static_cast<uint8_t>((maximum.b - minimum.b) / 16);
  minimum = {static_cast<uint8_t>(minimum.r + inset_r),
             static_cast<uint8_t>(minimum.g + inset_g),
             static_cast<uint8_t>(minimum.b + inset_b)};
  maximum = {static_cast<uint8_t>(maximum.r - inset_r),
             static_cast<uint8_t>(maximum.g - inset_g),
             static_cast<uint8_t>(maximum.b - inset_b)};

  uint16_t endpoint0 = PackRgb565(maximum);
  uint16_t endpoint1 = PackRgb565(minimum);
  if (endpoint0 < endpoint1) {
    std::swap(endpoint0, endpoint1);
  }
  if (endpoint0 == endpoint1) {
    if (endpoint0 < std::numeric_limits<uint16_t>::max()) {
      ++endpoint0;
    } else {
      --endpoint1;
    }
  }
  const Rgb color0 = UnpackRgb565(endpoint0);
  const Rgb color1 = UnpackRgb565(endpoint1);
  const std::array<Rgb, 4> palette = {
      color0,
      color1,
      Rgb{static_cast<uint8_t>((2 * color0.r + color1.r) / 3),
          static_cast<uint8_t>((2 * color0.g + color1.g) / 3),
          static_cast<uint8_t>((2 * color0.b + color1.b) / 3)},
      Rgb{static_cast<uint8_t>((color0.r + 2 * color1.r) / 3),
          static_cast<uint8_t>((color0.g + 2 * color1.g) / 3),
          static_cast<uint8_t>((color0.b + 2 * color1.b) / 3)},
  };
  uint32_t indices = 0;
  for (size_t pixel_index = 0; pixel_index < pixels.size(); ++pixel_index) {
    uint32_t best_distance = std::numeric_limits<uint32_t>::max();
    uint32_t best_index = 0;
    for (uint32_t palette_index = 0; palette_index < palette.size();
         ++palette_index) {
      const uint32_t distance =
          ColorDistance(pixels[pixel_index], palette[palette_index]);
      if (distance < best_distance) {
        best_distance = distance;
        best_index = palette_index;
      }
    }
    indices |= best_index << (pixel_index * 2);
  }
  bc1[0] = static_cast<uint8_t>(endpoint0);
  bc1[1] = static_cast<uint8_t>(endpoint0 >> 8);
  bc1[2] = static_cast<uint8_t>(endpoint1);
  bc1[3] = static_cast<uint8_t>(endpoint1 >> 8);
  WriteLe32(bc1 + 4, indices);
}

std::mutex& TranscodeMemoryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::vector<unsigned char>>&
TranscodeMemory() {
  static std::unordered_map<std::string, std::vector<unsigned char>> memo;
  return memo;
}

bool TranscodeEtc1SkyTextureForVulkan(const char *path,
                                      std::vector<unsigned char> *data) {
  if (!IsBundledSkyTexture(path) || data == nullptr ||
      data->size() > kMaxSkyTextureSize) {
    return false;
  }

  const std::string path_key = path != nullptr ? path : "";
  {
    std::lock_guard<std::mutex> lock(TranscodeMemoryMutex());
    auto it = TranscodeMemory().find(path_key);
    if (it != TranscodeMemory().end()) {
      *data = it->second;
      return true;
    }
  }

  // The bundled sky textures are payload files, so their converted form only
  // changes when the payload does. The directory is keyed by file identity, not
  // by name, and every field of that identity has to agree before an entry is
  // used; anything else is converted again and simply costs the launch the
  // conversion it would have paid anyway.
  const std::string directory = CacheDirectory();
  SourceIdentity identity;
  const bool cacheable =
      !directory.empty() && ReadSourceIdentity(path, &identity);
  const std::string cache_file =
      cacheable ? directory + "/" + CacheFileName(path_key, identity)
                : std::string();
  if (cacheable && LoadCachedTranscode(cache_file, identity, data)) {
    std::lock_guard<std::mutex> lock(TranscodeMemoryMutex());
    TranscodeMemory()[path_key] = *data;
    std::fprintf(stderr,
                 "  [asset] Reused cached BC1 sky texture for Vulkan: %s\n",
                 path);
    return true;
  }

  if (!TranscodeKtx1Etc1(data)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(TranscodeMemoryMutex());
    TranscodeMemory()[path_key] = *data;
  }
  if (cacheable) {
    (void)StoreCachedTranscode(cache_file, identity, *data);
  }

  std::fprintf(stderr,
               "  [asset] Transcoded bundled ETC1 sky texture to BC1 for "
               "Vulkan: %s\n",
               path);
  return true;
}

void ResetEtc1SkyTranscodeMemoryCache() {
  std::lock_guard<std::mutex> lock(TranscodeMemoryMutex());
  TranscodeMemory().clear();
}

} // namespace libc_shim
