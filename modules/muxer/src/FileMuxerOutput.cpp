/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MuxerOutput.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace coral {
namespace muxer {
namespace {

class FileMuxerOutput final : public MuxerOutput {
public:
  explicit FileMuxerOutput(std::string path) noexcept
      : mPath(std::move(path)) {
    mStream.open(mPath, std::ios::binary | std::ios::trunc);
  }

  [[nodiscard]] bool isOpen() const {
    return mStream.is_open();
  }

  ~FileMuxerOutput() override {
    FileMuxerOutput::close();
  }

  bool write(const uint8_t* data, const size_t size) override {
    if (!mStream.is_open() || (data == nullptr && size > 0)) {
      return false;
    }
    mStream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return mStream.good();
  }

  bool seek(const int64_t position) override {
    if (!mStream.is_open() || position < 0) {
      return false;
    }
    mStream.seekp(static_cast<std::streamoff>(position));
    return mStream.good();
  }

  [[nodiscard]] int64_t position() const override {
    if (!mStream.is_open()) {
      return -1;
    }
    return static_cast<int64_t>(mStream.tellp());
  }

  [[nodiscard]] bool isSeekable() const override {
    return mStream.is_open();
  }

  bool truncate(const int64_t size) override {
    if (!mStream.is_open() || size < 0) {
      return false;
    }
    mStream.flush();
    std::error_code error;
    std::filesystem::resize_file(mPath, static_cast<uintmax_t>(size), error);
    return !error;
  }

  bool copyBytes(const int64_t destination, const int64_t source, const int64_t size) override {
    if (!mStream.is_open() || destination < 0 || source < 0 || size < 0) {
      return false;
    }
    if (size == 0) {
      return true;
    }
    mStream.flush();
    std::ifstream reader(mPath, std::ios::binary);
    if (!reader.is_open()) {
      return false;
    }

    constexpr size_t kBufferSize = 1 << 20;
    std::vector<uint8_t> buffer(kBufferSize);
    // A forward copy would overwrite the source when the destination starts after the source, so
    // copy from the end in that case.
    const bool copyBackwards = destination > source && destination < source + size;
    int64_t remaining = size;
    while (remaining > 0) {
      const auto chunk = static_cast<size_t>(
        std::min<int64_t>(remaining, static_cast<int64_t>(kBufferSize)));
      const int64_t offsetInRange = copyBackwards ? remaining - static_cast<int64_t>(chunk) : 0;
      const int64_t readPosition = source + offsetInRange;
      const int64_t writePosition = destination + offsetInRange;

      reader.clear();
      reader.seekg(static_cast<std::streamoff>(readPosition));
      reader.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk));
      if (reader.gcount() != static_cast<std::streamsize>(chunk)) {
        return false;
      }

      mStream.clear();
      mStream.seekp(static_cast<std::streamoff>(writePosition));
      mStream.write(reinterpret_cast<const char*>(buffer.data()),
        static_cast<std::streamsize>(chunk));
      if (!mStream.good()) {
        return false;
      }
      remaining -= static_cast<int64_t>(chunk);
    }
    mStream.flush();
    return mStream.good();
  }

  bool close() override {
    if (!mStream.is_open()) {
      return true;
    }
    mStream.flush();
    mStream.close();
    return !mStream.fail();
  }

  void remove() override {
    close();
    std::error_code error;
    std::filesystem::remove(mPath, error);
  }

private:
  std::string mPath;
  mutable std::ofstream mStream;
};

} // namespace

std::unique_ptr<MuxerOutput> openFileOutput(const std::string& path, std::string* error) {
  auto output = std::make_unique<FileMuxerOutput>(path);
  if (!output->isOpen()) {
    if (error != nullptr) {
      *error = "cannot open the output file: " + path;
    }
    return nullptr;
  }
  return output;
}

} // namespace muxer
} // namespace coral
