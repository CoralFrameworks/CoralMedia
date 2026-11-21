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

#include <extractor/metadata/Id3Parser.h>

#include <media/Definitions.h>
#include <media/MetadataEntries.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr size_t kTagHeaderSize = 10;
constexpr size_t kFooterSize = 10;
constexpr uint8_t kFlagUnsynchronisation = 0x80;
constexpr uint8_t kFlagExtendedHeader = 0x40;
constexpr uint8_t kFlagFooter = 0x10;

void appendUtf8(std::string& out, const uint32_t codePoint) {
  if (codePoint <= 0x7F) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

/** Decodes ISO-8859-1, which is the encoding of an ID3 frame for the characters it defines. */
std::string decodeLatin1(const uint8_t* data, const size_t size) {
  std::string text;
  text.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    appendUtf8(text, data[i]);
  }
  return text;
}

/** Decodes UTF-16, honouring the byte order mark when there is one. */
std::string decodeUtf16(const uint8_t* data, size_t size, const bool defaultBigEndian) {
  bool bigEndian = defaultBigEndian;
  if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
    bigEndian = false;
    data += 2;
    size -= 2;
  } else if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    bigEndian = true;
    data += 2;
    size -= 2;
  }
  std::string text;
  size_t offset = 0;
  while (offset + 2 <= size) {
    const uint32_t unit = bigEndian
                            ? (static_cast<uint32_t>(data[offset]) << 8) | data[offset + 1]
                            : static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8);
    offset += 2;
    if (unit == 0) {
      break;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF && offset + 2 <= size) {
      const uint32_t low = bigEndian
                             ? (static_cast<uint32_t>(data[offset]) << 8) | data[offset + 1]
                             : static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        offset += 2;
        appendUtf8(text, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
        continue;
      }
    }
    appendUtf8(text, unit);
  }
  return text;
}

/**
 * Decodes one string of a frame.
 *
 * @param data The bytes of the frame.
 * @param size The number of bytes available.
 * @param encoding The encoding byte of the frame.
 * @param[out] consumed The number of bytes the string and its terminator took.
 *
 * @return The decoded string.
 */
std::string decodeString(const uint8_t* data, const size_t size, const uint8_t encoding,
                         size_t& consumed) {
  consumed = 0;
  if (data == nullptr || size == 0) {
    return {};
  }
  switch (encoding) {
    case 1: {
      // UTF-16 with a byte order mark, and the terminator is a 16 bit zero.
      size_t offset = 0;
      bool bigEndian = false;
      if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        bigEndian = true;
      }
      while (offset + 2 <= size) {
        const uint32_t unit = bigEndian
                                ? (static_cast<uint32_t>(data[offset]) << 8) | data[offset + 1]
                                : static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8);
        if (unit == 0) {
          consumed = offset + 2;
          return decodeUtf16(data, offset, bigEndian);
        }
        offset += 2;
      }
      consumed = size;
      return decodeUtf16(data, size, bigEndian);
    }
    case 2: {
      size_t offset = 0;
      while (offset + 2 <= size) {
        if (data[offset] == 0 && data[offset + 1] == 0) {
          consumed = offset + 2;
          return decodeUtf16(data, offset, true);
        }
        offset += 2;
      }
      consumed = size;
      return decodeUtf16(data, size, true);
    }
    case 3: {
      const void* terminator = std::memchr(data, 0, size);
      const size_t length = terminator == nullptr
                              ? size
                              : static_cast<size_t>(static_cast<const uint8_t*>(terminator) - data);
      consumed = terminator == nullptr ? size : length + 1;
      return std::string(reinterpret_cast<const char*>(data), length);
    }
    case 0:
    default: {
      const void* terminator = std::memchr(data, 0, size);
      const size_t length = terminator == nullptr
                              ? size
                              : static_cast<size_t>(static_cast<const uint8_t*>(terminator) - data);
      consumed = terminator == nullptr ? size : length + 1;
      return decodeLatin1(data, length);
    }
  }
}

/** Decodes every null separated value of a text frame, returning the first one. */
std::string decodeFirstValue(const uint8_t* data, const size_t size, const uint8_t encoding) {
  size_t consumed = 0;
  return decodeString(data, size, encoding, consumed);
}

/** Removes the zero byte that the unsynchronisation of a tag inserted after every 0xFF. */
std::vector<uint8_t> deUnsynchronise(const uint8_t* data, const size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(data[i]);
    if (data[i] == 0xFF && i + 1 < size && data[i + 1] == 0x00) {
      ++i;
    }
  }
  return out;
}

bool readSynchsafe(const uint8_t* data, const size_t size, size_t& value) {
  if (data == nullptr || size < 4) {
    return false;
  }
  if ((data[0] | data[1] | data[2] | data[3]) & 0x80) {
    return false;
  }
  value = (static_cast<size_t>(data[0]) << 21) | (static_cast<size_t>(data[1]) << 14) |
          (static_cast<size_t>(data[2]) << 7) | static_cast<size_t>(data[3]);
  return true;
}

uint32_t readUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

/** Returns whether @p data starts a frame identifier: four upper case letters or digits. */
bool isFrameIdentifier(const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const uint8_t character = data[i];
    const bool valid = (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
    if (!valid) {
      return false;
    }
  }
  return true;
}

/** Maps a version 2.2 frame identifier to its version 2.3 equivalent, or returns it unchanged. */
std::string normalizeFrameId(const std::string& id) {
  if (id.size() != 3) {
    return id;
  }
  static const struct {
    const char* from;
    const char* to;
  } kMappings[] = {
    {"TT1", "TIT1"}, {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
    {"TP3", "TPE3"}, {"TP4", "TPE4"}, {"TAL", "TALB"}, {"TRK", "TRCK"}, {"TYE", "TYER"},
    {"TDA", "TDAT"}, {"TCO", "TCON"}, {"TCM", "TCOM"}, {"TEN", "TENC"}, {"TSS", "TSSE"},
    {"TXX", "TXXX"}, {"WXX", "WXXX"}, {"COM", "COMM"}, {"ULT", "USLT"}, {"PIC", "APIC"},
    {"GEO", "GEOB"}, {"CNT", "PCNT"}, {"POP", "POPM"}, {"BUF", "RBUF"}, {"CRA", "AENC"},
    {"ETC", "ETCO"}, {"MLL", "MLLT"}, {"STC", "SYTC"}, {"SLT", "SYLT"}, {"RVA", "RVAD"},
    {"EQU", "EQUA"}, {"REV", "RVRB"}, {"TBP", "TBPM"}, {"TCR", "TCOP"}, {"TDY", "TDLY"},
    {"TFT", "TFLT"}, {"TKE", "TKEY"}, {"TLA", "TLAN"}, {"TLE", "TLEN"}, {"TMT", "TMED"},
    {"TOA", "TOPE"}, {"TOF", "TOFN"}, {"TOL", "TOLY"}, {"TOR", "TORY"}, {"TOT", "TOAL"},
    {"TPB", "TPUB"}, {"TSI", "TSIZ"}, {"TSP", "TSP"},
  };
  for (const auto& mapping : kMappings) {
    if (id == mapping.from) {
      return mapping.to;
    }
  }
  return id;
}

/** Appends one frame to @p entries. */
void appendFrame(const std::string& frameId, const uint8_t* payload, const size_t size,
                 const std::shared_ptr<Metadata>& metadata) {
  const std::string id = normalizeFrameId(frameId);
  std::string description;
  std::string language;
  std::string text;
  std::vector<uint8_t> body;

  if (id.size() == 4 && id[0] == 'T') {
    if (size == 0) {
      return;
    }
    const uint8_t encoding = payload[0];
    if (id == "TXXX") {
      size_t consumed = 0;
      description = decodeString(payload + 1, size - 1, encoding, consumed);
      if (1 + consumed < size) {
        text = decodeString(payload + 1 + consumed, size - 1 - consumed, encoding, consumed);
      }
    } else {
      text = decodeFirstValue(payload + 1, size - 1, encoding);
    }
  } else if (id.size() == 4 && id[0] == 'W') {
    if (size == 0) {
      return;
    }
    if (id == "WXXX") {
      const uint8_t encoding = payload[0];
      size_t consumed = 0;
      description = decodeString(payload + 1, size - 1, encoding, consumed);
      if (1 + consumed < size) {
        text = decodeLatin1(payload + 1 + consumed, size - 1 - consumed);
      }
    } else {
      text = decodeLatin1(payload, size);
    }
  } else if (id == "COMM" || id == "USLT") {
    if (size < 4) {
      return;
    }
    const uint8_t encoding = payload[0];
    language.assign(reinterpret_cast<const char*>(payload) + 1, 3);
    size_t consumed = 0;
    description = decodeString(payload + 4, size - 4, encoding, consumed);
    if (4 + consumed < size) {
      text = decodeString(payload + 4 + consumed, size - 4 - consumed, encoding, consumed);
    }
  } else if (id == "PRIV") {
    const void* terminator = std::memchr(payload, 0, size);
    const size_t ownerSize = terminator == nullptr
                               ? size
                               : static_cast<size_t>(static_cast<const uint8_t*>(terminator) - payload);
    description = decodeLatin1(payload, ownerSize);
    if (ownerSize + 1 <= size) {
      body.assign(payload + ownerSize + 1, payload + size);
    }
  } else if (id == "APIC") {
    if (size < 4) {
      return;
    }
    const uint8_t encoding = payload[0];
    const void* mimeEnd = std::memchr(payload + 1, 0, size - 1);
    if (mimeEnd == nullptr) {
      return;
    }
    const size_t mimeSize = static_cast<size_t>(static_cast<const uint8_t*>(mimeEnd) - (payload + 1));
    description = decodeLatin1(payload + 1, mimeSize);
    size_t offset = 1 + mimeSize + 1 + 1;  // encoding, mime, terminator, picture type
    if (offset > size) {
      return;
    }
    size_t consumed = 0;
    text = decodeString(payload + offset, size - offset, encoding, consumed);
    offset += consumed;
    if (offset <= size) {
      body.assign(payload + offset, payload + size);
    }
  } else {
    body.assign(payload, payload + size);
  }

  metadata->appendedEntries({std::make_shared<Id3FrameMetadataEntry>(
    frameId, std::move(description), std::move(language), std::move(text), std::move(body))});
}

} // namespace

bool AVId3Parser::isId3(const uint8_t* data, const size_t size) {
  return data != nullptr && size >= kTagHeaderSize && data[0] == 'I' && data[1] == 'D' && data[2] == '3' &&
         data[3] != 0xFF && data[4] != 0xFF;
}

size_t AVId3Parser::getTagSize(const uint8_t* data, const size_t size) {
  if (!isId3(data, size)) {
    return 0;
  }
  size_t bodySize = 0;
  if (!readSynchsafe(data + 6, size - 6, bodySize)) {
    return 0;
  }
  const uint8_t major = data[3];
  const uint8_t flags = data[5];
  size_t total = kTagHeaderSize + bodySize;
  if (major == 4 && (flags & kFlagFooter) != 0) {
    total += kFooterSize;
  }
  if (total > size || total < kTagHeaderSize) {
    return 0;
  }
  return total;
}

size_t AVId3Parser::parse(const uint8_t* data, const size_t size,
                          const std::shared_ptr<Metadata>& metadata) {
  const size_t tagSize = getTagSize(data, size);
  if (tagSize == 0 || metadata == nullptr) {
    return tagSize;
  }
  const uint8_t major = data[3];
  const uint8_t flags = data[5];
  size_t bodySize = tagSize - kTagHeaderSize;
  if (major == 4 && (flags & kFlagFooter) != 0) {
    bodySize -= kFooterSize;
  }
  const uint8_t* body = data + kTagHeaderSize;
  std::vector<uint8_t> unsynchronised;
  if ((flags & kFlagUnsynchronisation) != 0) {
    unsynchronised = deUnsynchronise(body, bodySize);
    body = unsynchronised.data();
    bodySize = unsynchronised.size();
  }

  size_t offset = 0;
  if ((flags & kFlagExtendedHeader) != 0) {
    if (major == 3 && bodySize >= 4) {
      offset = 4 + static_cast<size_t>(readUInt32(body));
    } else if (major == 4) {
      size_t extendedSize = 0;
      if (readSynchsafe(body, bodySize, extendedSize)) {
        offset = extendedSize;
      }
    }
    offset = std::min(offset, bodySize);
  }

  const size_t identifierSize = major == 2 ? 3 : 4;
  const size_t headerSize = major == 2 ? 6 : 10;
  const size_t sizeSize = major == 2 ? 3 : 4;
  while (offset + headerSize <= bodySize) {
    if (!isFrameIdentifier(body + offset, identifierSize) || body[offset] == 0) {
      // Padding, or a frame identifier that the tag does not define.
      break;
    }
    const std::string frameId(reinterpret_cast<const char*>(body) + offset, identifierSize);
    size_t frameSize = 0;
    if (major == 2) {
      frameSize = (static_cast<size_t>(body[offset + 3]) << 16) |
                  (static_cast<size_t>(body[offset + 4]) << 8) | body[offset + 5];
    } else if (major == 3) {
      frameSize = readUInt32(body + offset + 4);
    } else {
      if (!readSynchsafe(body + offset + 4, 4, frameSize)) {
        break;
      }
    }
    const size_t frameHeaderSize = headerSize;
    if (frameSize == 0 || frameSize > bodySize - offset - frameHeaderSize) {
      break;
    }
    const uint8_t* payload = body + offset + frameHeaderSize;
    size_t payloadSize = frameSize;
    if (major >= 3) {
      // The first flags byte of a frame is its status (tag alter, file alter, read only) and the
      // second one its format (compression, encryption, grouping, ...).
      const uint8_t formatFlags = body[offset + 9];
      if (major == 3) {
        if ((formatFlags & 0x80) != 0 || (formatFlags & 0x40) != 0) {
          // Compressed or encrypted: the content cannot be decoded.
          offset += frameHeaderSize + frameSize;
          continue;
        }
        if ((formatFlags & 0x20) != 0 && payloadSize > 0) {
          ++payload;
          --payloadSize;
        }
      } else {
        if ((formatFlags & 0x08) != 0 || (formatFlags & 0x04) != 0) {
          offset += frameHeaderSize + frameSize;
          continue;
        }
        if ((formatFlags & 0x40) != 0 && payloadSize > 0) {
          ++payload;
          --payloadSize;
        }
        if ((formatFlags & 0x01) != 0 && payloadSize >= 4) {
          payload += 4;
          payloadSize -= 4;
        }
        if ((formatFlags & 0x02) != 0) {
          const std::vector<uint8_t> frame = deUnsynchronise(payload, payloadSize);
          appendFrame(frameId, frame.data(), frame.size(), metadata);
          offset += frameHeaderSize + frameSize;
          continue;
        }
      }
    }
    appendFrame(frameId, payload, payloadSize, metadata);
    offset += frameHeaderSize + frameSize;
  }
  return tagSize;
}

size_t AVId3Parser::parseAll(const uint8_t* data, size_t size,
                             const std::shared_ptr<Metadata>& metadata) {
  size_t total = 0;
  while (size > 0) {
    const size_t tagSize = parse(data, size, metadata);
    if (tagSize == 0) {
      break;
    }
    total += tagSize;
    data += tagSize;
    size -= tagSize;
    if (size >= 3 && std::memcmp(data, "ID3", 3) == 0) {
      continue;
    }
    break;
  }
  return total;
}

std::string AVId3Parser::findText(const std::shared_ptr<Metadata>& metadata,
                                  const std::string& frameId) {
  if (metadata == nullptr) {
    return {};
  }
  const std::string wanted = normalizeFrameId(frameId);
  for (const auto& entry : metadata->getEntries()) {
    const auto* frame = dynamic_cast<Id3FrameMetadataEntry*>(entry.get());
    if (frame != nullptr && normalizeFrameId(frame->getFrameId()) == wanted) {
      return frame->getText();
    }
  }
  return {};
}

} // namespace coral
