// Copyright (C) 2026 Vorlie
// This file is part of Iota Studio.
//
// Iota Studio is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Iota Studio is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
// See the LICENSE file for the full license text.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "iota/core/zip_archive.hpp"

#include <atomic>
#include <cstring>
#include <fstream>
#include <miniz.h>
#include <system_error>

namespace iota::core {

namespace {
std::atomic<uint64_t> gZipTmpCounter{0};
}  // namespace

bool isZipSignature(std::span<const uint8_t> data) noexcept {
  return data.size() >= 2 && data[0] == 0x50 && data[1] == 0x4B;
}

std::optional<std::vector<ZipEntryData>> readZipFromMemory(
    std::span<const uint8_t> data,
    std::string* outError) {
  mz_zip_archive zip{};
  mz_zip_zero_struct(&zip);
  if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
    if (outError) *outError = "Failed to initialize miniz reader from memory";
    return std::nullopt;
  }

  std::vector<ZipEntryData> entries;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  entries.reserve(count);

  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

    size_t uncompSize = 0;
    void* ptr = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
    if (!ptr) {
      if (outError) *outError = std::string("Failed extracting ZIP entry: ") + st.m_filename;
      mz_zip_reader_end(&zip);
      return std::nullopt;
    }

    ZipEntryData entry;
    entry.name = st.m_filename;
    entry.bytes.resize(uncompSize);
    if (uncompSize > 0) {
      std::memcpy(entry.bytes.data(), ptr, uncompSize);
    }
    mz_free(ptr);
    entries.push_back(std::move(entry));
  }

  mz_zip_reader_end(&zip);
  return entries;
}

std::optional<std::vector<ZipEntryData>> readZipFromFile(
    const std::filesystem::path& path,
    std::string* outError) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    if (outError) *outError = "ZIP path is not a regular file";
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (outError) *outError = "Cannot open file for ZIP read";
    return std::nullopt;
  }
  const auto sz = in.tellg();
  if (sz < 0) {
    if (outError) *outError = "Failed determining ZIP file size";
    return std::nullopt;
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz)) {
    if (outError) *outError = "Failed reading ZIP file bytes";
    return std::nullopt;
  }
  return readZipFromMemory(buf, outError);
}

bool writeZipToMemory(
    const std::vector<ZipEntryData>& entries,
    std::vector<uint8_t>& outBytes,
    std::string* outError) {
  mz_zip_archive zip{};
  mz_zip_zero_struct(&zip);
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    if (outError) *outError = "Failed initializing miniz heap writer";
    return false;
  }

  for (const auto& e : entries) {
    if (!mz_zip_writer_add_mem(&zip,
                               e.name.c_str(),
                               e.bytes.data(),
                               e.bytes.size(),
                               static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION))) {
      if (outError) *outError = "Failed adding ZIP entry: " + e.name;
      mz_zip_writer_end(&zip);
      return false;
    }
  }

  void* buf = nullptr;
  size_t sz = 0;
  if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz)) {
    if (outError) *outError = "Failed finalizing ZIP archive";
    mz_zip_writer_end(&zip);
    return false;
  }

  outBytes.resize(sz);
  if (sz > 0 && buf) {
    std::memcpy(outBytes.data(), buf, sz);
  }
  mz_free(buf);
  mz_zip_writer_end(&zip);
  return true;
}

bool writeZipToFile(
    const std::filesystem::path& path,
    const std::vector<ZipEntryData>& entries,
    std::string* outError) {
  std::vector<uint8_t> bytes;
  if (!writeZipToMemory(entries, bytes, outError)) return false;

  std::filesystem::path tmpPath = path;
  tmpPath += ".tmp." + std::to_string(gZipTmpCounter.fetch_add(1, std::memory_order_relaxed));

  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (outError) *outError = "Cannot open output file for ZIP write";
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code rmEc;
      std::filesystem::remove(tmpPath, rmEc);
      if (outError) *outError = "Failed writing ZIP file";
      return false;
    }
  }

  std::error_code renameEc;
  std::filesystem::rename(tmpPath, path, renameEc);
  if (renameEc) {
    std::error_code rmEc;
    std::filesystem::remove(tmpPath, rmEc);
    if (outError) *outError = "Failed finalizing ZIP file rename: " + renameEc.message();
    return false;
  }
  return true;
}

} // namespace iota::core
