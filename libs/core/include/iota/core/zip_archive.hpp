#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iota::core {

struct ZipEntryData {
  std::string name;
  std::vector<uint8_t> bytes;
};

bool isZipSignature(std::span<const uint8_t> data) noexcept;

std::optional<std::vector<ZipEntryData>> readZipFromMemory(
    std::span<const uint8_t> data,
    std::string* outError = nullptr);

std::optional<std::vector<ZipEntryData>> readZipFromFile(
    const std::filesystem::path& path,
    std::string* outError = nullptr);

bool writeZipToMemory(
    const std::vector<ZipEntryData>& entries,
    std::vector<uint8_t>& outBytes,
    std::string* outError = nullptr);

bool writeZipToFile(
    const std::filesystem::path& path,
    const std::vector<ZipEntryData>& entries,
    std::string* outError = nullptr);

} // namespace iota::core
