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

#include <extractor/metadata/VorbisCommentParser.h>

#include <media/MetadataEntries.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr uint32_t kMaxCommentCount = 4096;
constexpr uint32_t kMaxCommentLength = 16 * 1024 * 1024;

uint32_t readLittleEndianUInt32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t readBigEndianUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

std::string toLowerAscii(const std::string& value) {
  std::string lower = value;
  for (char& character : lower) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lower;
}

} // namespace

size_t AVVorbisCommentParser::parse(const uint8_t* data, const size_t size,
                                    const std::shared_ptr<Metadata>& metadata) {
  if (data == nullptr || size < 4 || metadata == nullptr) {
    return 0;
  }
  size_t offset = 0;
  const uint32_t vendorLength = readLittleEndianUInt32(data);
  offset += 4;
  if (vendorLength > size - offset) {
    return 0;
  }
  const std::string vendor(reinterpret_cast<const char*>(data) + offset, vendorLength);
  offset += vendorLength;
  if (vendorLength > 0) {
    metadata->appendedEntries(
      {std::make_shared<TextMetadataEntry>("vendor", vendor)});
  }
  if (offset + 4 > size) {
    return 0;
  }
  const uint32_t commentCount = std::min(readLittleEndianUInt32(data + offset), kMaxCommentCount);
  offset += 4;
  for (uint32_t i = 0; i < commentCount; ++i) {
    if (offset + 4 > size) {
      return 0;
    }
    const uint32_t commentLength = readLittleEndianUInt32(data + offset);
    offset += 4;
    if (commentLength > kMaxCommentLength || commentLength > size - offset) {
      return 0;
    }
    const std::string comment(reinterpret_cast<const char*>(data) + offset, commentLength);
    offset += commentLength;
    const size_t equals = comment.find('=');
    const std::string key =
      toLowerAscii(equals == std::string::npos ? comment : comment.substr(0, equals));
    const std::string value = equals == std::string::npos ? std::string() : comment.substr(equals + 1);
    if (!key.empty()) {
      metadata->appendedEntries({std::make_shared<TextMetadataEntry>(key, value)});
    }
  }
  return offset;
}

size_t AVVorbisCommentParser::parseFlacPicture(const uint8_t* data, const size_t size,
                                               const std::shared_ptr<Metadata>& metadata) {
  if (data == nullptr || size < 32 || metadata == nullptr) {
    return 0;
  }
  size_t offset = 0;
  offset += 4;  // the picture type
  const uint32_t mimeLength = readBigEndianUInt32(data + offset);
  offset += 4;
  if (mimeLength > size - offset) {
    return 0;
  }
  const std::string mimeType(reinterpret_cast<const char*>(data) + offset, mimeLength);
  offset += mimeLength;
  if (offset + 4 > size) {
    return 0;
  }
  const uint32_t descriptionLength = readBigEndianUInt32(data + offset);
  offset += 4;
  if (descriptionLength > size - offset) {
    return 0;
  }
  const std::string description(reinterpret_cast<const char*>(data) + offset, descriptionLength);
  offset += descriptionLength;
  if (offset + 20 > size) {
    return 0;
  }
  const int width = static_cast<int>(readBigEndianUInt32(data + offset));
  const int height = static_cast<int>(readBigEndianUInt32(data + offset + 4));
  const int depth = static_cast<int>(readBigEndianUInt32(data + offset + 8));
  offset += 16;
  const uint32_t dataLength = readBigEndianUInt32(data + offset);
  offset += 4;
  if (dataLength > size - offset) {
    return 0;
  }
  std::vector<uint8_t> picture(data + offset, data + offset + dataLength);
  offset += dataLength;
  metadata->appendedEntries({std::make_shared<PictureMetadataEntry>(
    mimeType, description, width, height, depth, std::move(picture))});
  return offset;
}

size_t AVVorbisCommentParser::parseFlacCuesheet(const uint8_t* data, const size_t size,
                                                const std::shared_ptr<Metadata>& metadata) {
  if (data == nullptr || size == 0 || metadata == nullptr) {
    return 0;
  }
  metadata->appendedEntries({std::make_shared<BinaryMetadataEntry>(
    "flac.cuesheet", std::vector<uint8_t>(data, data + size))});
  return size;
}

} // namespace coral
