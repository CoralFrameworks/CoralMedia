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

#include <extractor/text/vobsub/VobSubParser.h>

#include "text/SubtitleTextUtil.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coral {
namespace {

uint16_t readU16(const uint8_t* data, size_t size, size_t offset) {
  if (offset + 2 > size) return 0;
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

struct NibbleReader final {
  const uint8_t* data;
  size_t size;
  size_t position = 0;
  bool high = false;

  bool read(uint8_t& value) {
    if (position >= size) return false;
    if (!high) {
      value = static_cast<uint8_t>(data[position] >> 4);
      high = true;
    } else {
      value = static_cast<uint8_t>(data[position] & 0x0F);
      high = false;
      ++position;
    }
    return true;
  }

  void byteAlign() {
    if (high) {
      high = false;
      ++position;
    }
  }
};

} // namespace

class AVVobSubParser::Impl final {
public:
  explicit Impl(const std::vector<std::vector<uint8_t>>& codecPrivate) {
    palette_.fill(0xFFFFFFFFu);
    alpha_.fill(0x0F);
    std::string text;
    for (const auto& part: codecPrivate) {
      text.append(reinterpret_cast<const char*>(part.data()), part.size());
      text.push_back('\n');
    }
    parseIndex(text);
  }

  void feed(const uint8_t* data, size_t size, std::vector<AVCuesWithTiming>& results) {
    std::vector<uint8_t> inflated;
    if (coral::text::maybeInflate(data, size, inflated)) {
      data = inflated.data();
      size = inflated.size();
    }
    if (size < 4) return;
    uint16_t packetSize = readU16(data, size, 0);
    if (packetSize == 0 || packetSize > size) packetSize = static_cast<uint16_t>(size);
    if (packetSize < 4) return;
    const size_t controlOffset = readU16(data, size, 2);
    if (controlOffset + 4 > packetSize) return;

    int x1 = 0;
    int y1 = 0;
    int x2 = width_ > 0 ? width_ - 1 : 0;
    int y2 = height_ > 0 ? height_ - 1 : 0;
    std::array<uint8_t, 4> paletteIndex{0, 1, 2, 3};
    std::array<uint8_t, 4> alphaIndex{0x0F, 0x0F, 0x0F, 0x0F};
    size_t evenOffset = 0;
    size_t oddOffset = 0;
    bool hasOffsets = false;
    int64_t startTimeUs = Definitions::kTimeUnset;
    int64_t endTimeUs = Definitions::kTimeUnset;

    // The control table is a chain of sequences. Each sequence states its time in 100ths of a
    // second, the offset of the next sequence, then its commands; this is the layout of a real SPU,
    // not a bare command list.
    size_t sequencePosition = controlOffset;
    size_t controlEnd = controlOffset;
    bool parsedSequence = false;
    while (sequencePosition + 4 <= packetSize) {
      const int64_t sequenceTimeUs = static_cast<int64_t>(readU16(data, packetSize, sequencePosition)) * 10000;
      const size_t nextSequence = readU16(data, packetSize, sequencePosition + 2);
      const bool hasNext = nextSequence > sequencePosition && nextSequence < packetSize;
      const size_t sequenceEnd = hasNext ? nextSequence : packetSize;
      controlEnd = sequenceEnd;
      size_t position = sequencePosition + 4;
      while (position < sequenceEnd) {
        const uint8_t command = data[position++];
        if (command == 0xFF) break;
        if (command == 0x00) {
          continue;
        }
        if (command == 0x01) {
          startTimeUs = sequenceTimeUs;
          continue;
        }
        if (command == 0x02) {
          endTimeUs = sequenceTimeUs;
          continue;
        }
        if (command == 0x03) {
          if (position + 2 > sequenceEnd) break;
          paletteIndex[3] = static_cast<uint8_t>(data[position] >> 4);
          paletteIndex[2] = static_cast<uint8_t>(data[position] & 0x0F);
          paletteIndex[1] = static_cast<uint8_t>(data[position + 1] >> 4);
          paletteIndex[0] = static_cast<uint8_t>(data[position + 1] & 0x0F);
          position += 2;
        } else if (command == 0x04) {
          if (position + 2 > sequenceEnd) break;
          alphaIndex[3] = static_cast<uint8_t>(data[position] >> 4);
          alphaIndex[2] = static_cast<uint8_t>(data[position] & 0x0F);
          alphaIndex[1] = static_cast<uint8_t>(data[position + 1] >> 4);
          alphaIndex[0] = static_cast<uint8_t>(data[position + 1] & 0x0F);
          position += 2;
        } else if (command == 0x05) {
          if (position + 6 > sequenceEnd) break;
          x1 = static_cast<int>((data[position] << 4) | (data[position + 1] >> 4));
          x2 = static_cast<int>(((data[position + 1] & 0x0F) << 8) | data[position + 2]);
          y1 = static_cast<int>((data[position + 3] << 4) | (data[position + 4] >> 4));
          y2 = static_cast<int>(((data[position + 4] & 0x0F) << 8) | data[position + 5]);
          position += 6;
        } else if (command == 0x06) {
          if (position + 4 > sequenceEnd) break;
          evenOffset = readU16(data, size, position);
          oddOffset = readU16(data, size, position + 2);
          hasOffsets = evenOffset > 0 && evenOffset < packetSize;
          position += 4;
        } else {
          break;
        }
      }
      parsedSequence = true;
      if (!hasNext) {
        break;
      }
      sequencePosition = nextSequence;
    }
    if (!parsedSequence) {
      return;
    }
    if (x2 < x1 || y2 < y1) return;
    const int width = x2 - x1 + 1;
    const int height = y2 - y1 + 1;
    if (width <= 0 || height <= 0) return;

    const size_t evenStart = hasOffsets ? evenOffset : controlEnd;
    const size_t oddStart = hasOffsets ? oddOffset : evenStart;
    if (evenStart >= packetSize) return;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
    decodeField(data, packetSize, evenStart, width, height, 0, paletteIndex, alphaIndex, pixels);
    decodeField(data, packetSize, oddStart, width, height, 1, paletteIndex, alphaIndex, pixels);

    AVCue cue;
    cue.bitmap.width = width;
    cue.bitmap.height = height;
    cue.bitmap.format = PixelFormat::kFormatRGBA;
    cue.bitmap.rowBytes = static_cast<uint32_t>(width) * 4;
    cue.bitmap.premultiplied = false;
    cue.bitmap.pixels = std::move(pixels);
    if (width_ > 0 && height_ > 0) {
      cue.position = static_cast<float>(x1) / static_cast<float>(width_);
      cue.line = static_cast<float>(y1) / static_cast<float>(height_);
      cue.size = static_cast<float>(width) / static_cast<float>(width_);
      cue.bitmapHeight = static_cast<float>(height) / static_cast<float>(height_);
      cue.positionAnchor = AVCue::AnchorType::kStart;
      cue.lineAnchor = AVCue::AnchorType::kStart;
      cue.lineType = AVCue::LineType::kFraction;
    }
    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = startTimeUs;
    result.durationUs = Definitions::kTimeUnset;
    if (endTimeUs != Definitions::kTimeUnset) {
      result.durationUs = startTimeUs != Definitions::kTimeUnset && endTimeUs > startTimeUs
                            ? endTimeUs - startTimeUs
                            : endTimeUs;
    }
    results.push_back(std::move(result));
  }

  void reset() {}

private:
  void parseIndex(const std::string& text) {
    size_t position = 0;
    while (position < text.size()) {
      size_t end = text.find('\n', position);
      if (end == std::string::npos) end = text.size();
      std::string line = coral::text::trim(std::string_view(text).substr(position, end - position));
      position = end + 1;
      const std::string lower = coral::text::toLowerAscii(line);
      if (lower.rfind("size:", 0) == 0) {
        const auto separator = line.find('x');
        if (separator != std::string::npos) {
          int64_t value = 0;
          if (coral::text::parseInteger(coral::text::trim(line.substr(5, separator - 5)), value)) {
            width_ = static_cast<int>(value);
          }
          if (coral::text::parseInteger(coral::text::trim(line.substr(separator + 1)), value)) {
            height_ = static_cast<int>(value);
          }
        }
      } else if (lower.rfind("palette:", 0) == 0) {
        const auto values = coral::text::split(line.substr(8), ',');
        for (size_t i = 0; i < values.size() && i < palette_.size(); ++i) {
          const std::string value = coral::text::trim(values[i]);
          uint32_t rgb = 0;
          bool valid = !value.empty();
          for (const char character: value) {
            int digit = -1;
            if (character >= '0' && character <= '9') digit = character - '0';
            else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
            else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
            else { valid = false; break; }
            rgb = (rgb << 4) | static_cast<uint32_t>(digit);
          }
          if (valid && value.size() == 6) palette_[i] = 0xFF000000u | rgb;
        }
      }
    }
  }

  void decodeField(const uint8_t* data, size_t size, size_t start, int width, int height,
      int field, const std::array<uint8_t, 4>& paletteIndex,
      const std::array<uint8_t, 4>& alphaIndex, std::vector<uint8_t>& pixels) const {
    if (start >= size || width <= 0 || height <= 0) return;
    NibbleReader reader{data + start, size - start};
    int x = 0;
    int y = field;
    while (y < height) {
      // A run is a sequence of nibbles; its low two bits select one of the four colours, and its
      // length is the width of the line for the short form (a value below four), or the remaining
      // value shifted by two for the long form.
      int value = 0;
      int test = 1;
      while (value < test && test <= 0x40) {
        uint8_t nibble = 0;
        if (!reader.read(nibble)) return;
        value = (value << 4) | nibble;
        test <<= 2;
      }
      const int color = value & 3;
      const int run = value < 4 ? width : (value >> 2);
      const int length = std::min(run, width - x);
      if (length <= 0) return;
      const uint8_t mapped = paletteIndex[static_cast<size_t>(color)];
      const uint32_t argb = palette_[mapped & 0x0F];
      const uint8_t alpha = static_cast<uint8_t>((alphaIndex[static_cast<size_t>(color)] & 0x0F) * 17);
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
      for (int i = 0; i < length; ++i) {
        pixels[offset + static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>((argb >> 16) & 0xFF);
        pixels[offset + static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
        pixels[offset + static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(argb & 0xFF);
        pixels[offset + static_cast<size_t>(i) * 4 + 3] = alpha;
      }
      x += length;
      if (x >= width) {
        y += 2;
        x = 0;
        reader.byteAlign();
      }
    }
  }

  std::array<uint32_t, 16> palette_{};
  std::array<uint8_t, 4> alpha_{};
  int width_ = 0;
  int height_ = 0;
};

AVVobSubParser::AVVobSubParser() : mImpl(new Impl({})) {
}

AVVobSubParser::AVVobSubParser(const std::vector<std::vector<uint8_t>>& codecPrivate)
  : mImpl(new Impl(codecPrivate)) {
}

AVVobSubParser::~AVVobSubParser() = default;

bool AVVobSubParser::parse(const uint8_t* data, size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) return false;
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVVobSubParser::reset() {
  mImpl->reset();
}

} // namespace coral
