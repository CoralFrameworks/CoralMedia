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

#include <extractor/metadata/MetadataDecoder.h>

#include <extractor/metadata/Id3Parser.h>

#include <media/MetadataEntries.h>
#include <media/Types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {
namespace {

/** A byte sequence that ends at the first zero byte. */
bool readNullTerminatedString(const uint8_t* data, const size_t size, size_t& offset,
                              std::string& out) {
  const size_t start = offset;
  while (offset < size && data[offset] != 0) {
    ++offset;
  }
  if (offset >= size) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(data) + start, offset - start);
  ++offset;  // the terminator
  return true;
}

uint32_t readUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint64_t readUInt64(const uint8_t* data) {
  return (static_cast<uint64_t>(readUInt32(data)) << 32) | readUInt32(data + 4);
}

/** The decoder of an ID3 tag list, e.g. the samples of an {@code application/id3} track. */
class AVId3MetadataDecoder final : public AVMetadataDecoder {
public:
  bool decode(const uint8_t* data, const size_t size,
              const std::shared_ptr<Metadata>& metadata) override {
    if (metadata == nullptr) {
      return true;
    }
    return AVId3Parser::parseAll(data, size, metadata) > 0;
  }
};

/** The decoder of an MP4 {@code emsg} box payload, e.g. the samples of an {@code application/x-emsg} track. */
class AVEmsgMetadataDecoder final : public AVMetadataDecoder {
public:
  bool decode(const uint8_t* data, const size_t size,
              const std::shared_ptr<Metadata>& metadata) override {
    if (data == nullptr || size < 4 || metadata == nullptr) {
      return false;
    }
    const uint8_t version = data[0];
    size_t offset = 4;
    std::string schemeIdUri;
    std::string value;
    uint32_t timescale = 0;
    uint64_t presentationTime = 0;
    uint32_t eventDuration = 0;
    uint32_t id = 0;
    if (version == 0) {
      if (!readNullTerminatedString(data, size, offset, schemeIdUri) ||
          !readNullTerminatedString(data, size, offset, value) || offset + 16 > size) {
        return false;
      }
      timescale = readUInt32(data + offset);
      presentationTime = readUInt32(data + offset + 4);
      eventDuration = readUInt32(data + offset + 8);
      id = readUInt32(data + offset + 12);
      offset += 16;
    } else if (version == 1) {
      if (offset + 20 > size) {
        return false;
      }
      timescale = readUInt32(data + offset);
      presentationTime = readUInt64(data + offset + 4);
      eventDuration = readUInt32(data + offset + 12);
      id = readUInt32(data + offset + 16);
      offset += 20;
      if (!readNullTerminatedString(data, size, offset, schemeIdUri) ||
          !readNullTerminatedString(data, size, offset, value)) {
        return false;
      }
    } else {
      return false;
    }
    (void)presentationTime;
    std::vector<uint8_t> messageData;
    if (offset < size) {
      messageData.assign(data + offset, data + size);
    }
    const int64_t durationUs =
      timescale > 0 ? static_cast<int64_t>(eventDuration) * 1000000 / timescale : 0;
    metadata->appendedEntries({std::make_shared<EmsgMetadataEntry>(
      std::move(schemeIdUri), std::move(value), durationUs, id, std::move(messageData))});
    return true;
  }
};

/** The decoder of a SCTE-35 splice information section. */
class AVScte35MetadataDecoder final : public AVMetadataDecoder {
public:
  bool decode(const uint8_t* data, const size_t size,
              const std::shared_ptr<Metadata>& metadata) override {
    // A splice information section starts with the table identifier 0xFC.
    if (data == nullptr || size < 4 || data[0] != 0xFC || metadata == nullptr) {
      return false;
    }
    metadata->appendedEntries(
      {std::make_shared<Scte35MetadataEntry>(std::vector<uint8_t>(data, data + size))});
    return true;
  }
};

class AVDefaultMetadataDecoderFactory final : public AVMetadataDecoderFactory {
public:
  [[nodiscard]] bool supportsFormat(const MediaFormat& format) const override {
    return format.getMimeType() == MimeTypes::APPLICATION_ID3 ||
           format.getMimeType() == MimeTypes::APPLICATION_EMSG ||
           format.getMimeType() == MimeTypes::APPLICATION_SCTE35;
  }

  [[nodiscard]] std::unique_ptr<AVMetadataDecoder> create(const MediaFormat& format) const override {
    if (format.getMimeType() == MimeTypes::APPLICATION_ID3) {
      return std::unique_ptr<AVMetadataDecoder>(new AVId3MetadataDecoder());
    }
    if (format.getMimeType() == MimeTypes::APPLICATION_EMSG) {
      return std::unique_ptr<AVMetadataDecoder>(new AVEmsgMetadataDecoder());
    }
    if (format.getMimeType() == MimeTypes::APPLICATION_SCTE35) {
      return std::unique_ptr<AVMetadataDecoder>(new AVScte35MetadataDecoder());
    }
    return nullptr;
  }
};

} // namespace

std::unique_ptr<AVMetadataDecoderFactory> createDefaultMetadataDecoderFactory() {
  return std::unique_ptr<AVMetadataDecoderFactory>(new AVDefaultMetadataDecoderFactory());
}

} // namespace coral
