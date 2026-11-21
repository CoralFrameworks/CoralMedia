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

#include <extractor/text/dvbsub/DvbSubParser.h>

#include "text/SubtitleTextUtil.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace coral {
namespace {

constexpr uint8_t kPageComposition = 0x10;
constexpr uint8_t kRegionComposition = 0x11;
constexpr uint8_t kClutDefinition = 0x12;
constexpr uint8_t kObjectData = 0x13;
constexpr uint8_t kDisplayDefinition = 0x14;
constexpr uint8_t kEndOfDisplaySet = 0x80;

uint16_t readU16(const uint8_t* data, size_t size, size_t offset) {
  if (offset + 2 > size) return 0;
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

uint8_t clampByte(const float value) {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

class BitReader final {
public:
  explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

  int read(const int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
      const size_t byte = position_ / 8;
      if (byte >= data_.size()) return value;
      value = (value << 1) | ((data_[byte] >> (7 - (position_ % 8))) & 1);
      ++position_;
    }
    return value;
  }

  void align() {
    position_ = (position_ + 7) & ~static_cast<size_t>(7);
  }

  [[nodiscard]] bool hasBits() const { return position_ < data_.size() * 8; }

private:
  const std::vector<uint8_t>& data_;
  size_t position_ = 0;
};

struct Region final {
  int id = 0;
  bool fill = false;
  int width = 0;
  int height = 0;
  int depth = 8;
  int clutId = 0;
  int pixelCode8 = 0;
  int pixelCode4 = 0;
  int pixelCode2 = 0;
  std::map<int, std::pair<int, int>> objects;
};

struct PageRegion final {
  int id = 0;
  int x = 0;
  int y = 0;
};

struct Object final {
  std::vector<uint8_t> top;
  std::vector<uint8_t> bottom;
};

struct Display final {
  int width = 0;
  int height = 0;
};

} // namespace

class AVDvbSubParser::Impl final {
private:
  struct Clut final {
    std::array<uint32_t, 4> entries2{};
    std::array<uint32_t, 16> entries4{};
    std::array<uint32_t, 256> entries8{};
    Clut() {
      entries2.fill(0xFF000000u);
      entries4.fill(0xFF000000u);
      entries8.fill(0xFF000000u);
      entries2[1] = entries4[1] = entries8[1] = 0xFFFFFFFFu;
      entries2[2] = entries4[2] = entries8[2] = 0xFFFF0000u;
      entries2[3] = entries4[3] = entries8[3] = 0xFF7F7F7Fu;
    }
  };


public:
  void feed(const uint8_t* data, size_t size, std::vector<AVCuesWithTiming>& results) {
    size_t position = 0;
    while (position + 6 <= size) {
      if (data[position] != 0x0F) {
        ++position;
        continue;
      }
      const uint8_t type = data[position + 1];
      const uint16_t pageId = readU16(data, size, position + 2);
      const uint16_t length = readU16(data, size, position + 4);
      const size_t payload = position + 6;
      if (payload + length > size) break;
      if (pageId == subtitlePageId_ || subtitlePageId_ < 0) {
        if (subtitlePageId_ < 0) subtitlePageId_ = pageId;
        switch (type) {
          case kDisplayDefinition: parseDisplay(data + payload, length); break;
          case kPageComposition: parsePage(data + payload, length); break;
          case kRegionComposition: parseRegion(data + payload, length); break;
          case kClutDefinition: parseClut(data + payload, length); break;
          case kObjectData: parseObject(data + payload, length); break;
          case kEndOfDisplaySet: emit(results); break;
          default: break;
        }
      }
      position = payload + length;
    }
    if (position >= size && !page_.empty()) emit(results);
  }

  void reset() {
    display_ = Display{};
    page_.clear();
    regions_.clear();
    objects_.clear();
    cluts_.clear();
    subtitlePageId_ = -1;
  }

private:
  void parseDisplay(const uint8_t* data, size_t size) {
    if (size < 5) return;
    display_.width = readU16(data, size, 1);
    display_.height = readU16(data, size, 3);
  }

  void parsePage(const uint8_t* data, size_t size) {
    if (size < 3) return;
    page_.clear();
    const int count = data[2];
    size_t position = 3;
    for (int i = 0; i < count && position + 6 <= size; ++i) {
      PageRegion region;
      region.id = data[position];
      region.x = readU16(data, size, position + 2);
      region.y = readU16(data, size, position + 4);
      page_.push_back(region);
      position += 6;
    }
  }

  void parseRegion(const uint8_t* data, size_t size) {
    if (size < 12) return;
    Region region;
    region.id = data[0];
    region.fill = (data[1] & 0x08) != 0;
    region.width = readU16(data, size, 2);
    region.height = readU16(data, size, 4);
    const int depthCode = data[7] & 0x07;
    region.depth = depthCode == 3 ? 8 : depthCode == 2 ? 4 : 2;
    region.clutId = data[8];
    region.pixelCode8 = data[9];
    region.pixelCode4 = data[10] & 0x0F;
    region.pixelCode2 = data[11] & 0x03;
    size_t position = 12;
    while (position + 6 <= size) {
      const int objectId = readU16(data, size, position);
      const int type = (data[position + 2] >> 6) & 0x03;
      const int x = ((data[position + 3] & 0x0F) << 8) | data[position + 4];
      const int y = ((data[position + 5] & 0x0F) << 8) | data[position + 6];
      position += 6;
      if (type == 1 || type == 2) position += 2;
      region.objects[objectId] = {x, y};
    }
    regions_[region.id] = std::move(region);
  }

  void parseClut(const uint8_t* data, size_t size) {
    if (size < 2) return;
    const int id = data[0];
    Clut& clut = cluts_[id];
    size_t position = 2;
    while (position + 2 <= size) {
      const int entryId = data[position];
      const int flags = data[position + 1];
      position += 2;
      const bool fullRange = (flags & 0x01) != 0;
      int y = 0;
      int cr = 0;
      int cb = 0;
      int t = 0;
      if (fullRange) {
        if (position + 4 > size) break;
        y = data[position++];
        cr = data[position++];
        cb = data[position++];
        t = data[position++];
      } else if (position + 3 <= size) {
        y = data[position++] & 0xFC;
        cr = data[position++] & 0xF0;
        cb = data[position++] & 0xF0;
        t = (data[position - 1] & 0x0F) * 17;
      } else {
        break;
      }
      const float red = y + 1.402f * (cr - 128.0f);
      const float green = y - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
      const float blue = y + 1.772f * (cb - 128.0f);
      const uint32_t argb = (static_cast<uint32_t>(t) << 24) |
                            (static_cast<uint32_t>(clampByte(red)) << 16) |
                            (static_cast<uint32_t>(clampByte(green)) << 8) |
                            clampByte(blue);
      if (flags & 0x80) clut.entries2[entryId & 0x03] = argb;
      else if (flags & 0x40) clut.entries4[entryId & 0x0F] = argb;
      else clut.entries8[entryId & 0xFF] = argb;
    }
  }

  void parseObject(const uint8_t* data, size_t size) {
    if (size < 4) return;
    const int id = readU16(data, size, 0);
    if (size < 8) return;
    const int topLength = readU16(data, size, 4);
    const int bottomLength = readU16(data, size, 6);
    size_t position = 8;
    Object object;
    if (position + topLength > size) return;
    object.top.assign(data + position, data + position + topLength);
    position += topLength;
    if (bottomLength > 0 && position + bottomLength <= size) {
      object.bottom.assign(data + position, data + position + bottomLength);
    } else {
      object.bottom = object.top;
    }
    objects_[id] = std::move(object);
  }

  void paint(BitReader& reader, const Clut& clut, int depth, std::vector<uint8_t>& pixels,
      int width, int height, int startColumn, int startLine = 0) {
    int column = startColumn;
    int line = startLine;
    bool end = false;
    while (!end && reader.hasBits()) {
      int run = 0;
      int color = 0;
      if (depth == 8) {
        const int peek = reader.read(8);
        if (peek != 0) { run = 1; color = peek; }
        else if (reader.read(1) == 0) {
          const int length = reader.read(7);
          if (length != 0) { run = length; }
          else end = true;
        } else {
          run = reader.read(7);
          color = reader.read(8);
        }
      } else if (depth == 4) {
        const int peek = reader.read(4);
        if (peek != 0) { run = 1; color = peek; }
        else if (reader.read(1) == 0) {
          const int length = reader.read(3);
          if (length != 0) { run = 2 + length; }
          else end = true;
        } else {
          run = reader.read(2);
          if (run == 0) { run = 1; }
          else if (run == 2) { run = 9 + reader.read(4); color = reader.read(4); }
          else if (run == 3) { run = 25 + reader.read(8); color = reader.read(4); }
        }
      } else {
        const int peek = reader.read(2);
        if (peek != 0) { run = 1; color = peek; }
        else if (reader.read(1) == 1) {
          run = 3 + reader.read(3);
          color = reader.read(2);
        } else if (reader.read(1) == 1) {
          run = 2;
        } else {
          const int type = reader.read(2);
          if (type == 0) end = true;
          else if (type == 1) run = 2;
          else if (type == 2) { run = 12 + reader.read(4); color = reader.read(2); }
          else { run = 29 + reader.read(8); color = reader.read(2); }
        }
      }
      if (run == 0) continue;
      const uint32_t argb = depth == 8 ? clut.entries8[color & 0xFF]
                              : depth == 4 ? clut.entries4[color & 0x0F]
                                           : clut.entries2[color & 0x03];
      const uint8_t alpha = static_cast<uint8_t>((argb >> 24) & 0xFF);
      const uint8_t red = static_cast<uint8_t>((argb >> 16) & 0xFF);
      const uint8_t green = static_cast<uint8_t>((argb >> 8) & 0xFF);
      const uint8_t blue = static_cast<uint8_t>(argb & 0xFF);
      for (int i = 0; i < run && column < width; ++i, ++column) {
        if (line < 0 || line >= height) continue;
        const size_t offset = (static_cast<size_t>(line) * width + column) * 4;
        pixels[offset + 0] = red;
        pixels[offset + 1] = green;
        pixels[offset + 2] = blue;
        pixels[offset + 3] = alpha;
      }
      if (column >= width) {
        column = 0;
        line += 2;
        reader.align();
      }
    }
  }

  void emit(std::vector<AVCuesWithTiming>& results) {
    if (page_.empty()) {
      return;
    }
    // ETSI EN 300 743 leaves Display Definition optional; the reference decoder falls back to
    // 720x576, so a stream without one must still produce cues.
    const int displayWidth = display_.width > 0 ? display_.width : 720;
    const int displayHeight = display_.height > 0 ? display_.height : 576;
    AVCuesWithTiming result;
    result.startTimeUs = Definitions::kTimeUnset;
    result.durationUs = Definitions::kTimeUnset;
    for (const auto& pageRegion: page_) {
      const auto found = regions_.find(pageRegion.id);
      if (found == regions_.end()) continue;
      const Region& region = found->second;
      if (region.width <= 0 || region.height <= 0) continue;
      const auto clutFound = cluts_.find(region.clutId);
      const Clut clut = clutFound == cluts_.end() ? Clut{} : clutFound->second;
      AVCue cue;
      cue.bitmap.width = region.width;
      cue.bitmap.height = region.height;
      cue.bitmap.format = PixelFormat::kFormatRGBA;
      cue.bitmap.rowBytes = static_cast<uint32_t>(region.width) * 4;
      cue.bitmap.premultiplied = false;
      cue.bitmap.pixels.assign(static_cast<size_t>(region.width) * region.height * 4, 0);
      if (region.fill) {
        const uint32_t argb = region.depth == 8 ? clut.entries8[region.pixelCode8 & 0xFF]
                                : region.depth == 4 ? clut.entries4[region.pixelCode4 & 0x0F]
                                                   : clut.entries2[region.pixelCode2 & 0x03];
        for (size_t i = 0; i < cue.bitmap.pixels.size(); i += 4) {
          cue.bitmap.pixels[i + 0] = static_cast<uint8_t>((argb >> 16) & 0xFF);
          cue.bitmap.pixels[i + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
          cue.bitmap.pixels[i + 2] = static_cast<uint8_t>(argb & 0xFF);
          cue.bitmap.pixels[i + 3] = static_cast<uint8_t>((argb >> 24) & 0xFF);
        }
      }
      for (const auto& [objectId, position]: region.objects) {
        const auto objectFound = objects_.find(objectId);
        if (objectFound == objects_.end()) continue;
        BitReader top(objectFound->second.top);
        paint(top, clut, region.depth, cue.bitmap.pixels, region.width, region.height,
          position.first);
        if (!objectFound->second.bottom.empty()) {
          BitReader bottom(objectFound->second.bottom);
          // The bottom field starts one line below the top field; starting both at line 0 made the
          // bottom field overwrite the top one.
          paint(bottom, clut, region.depth, cue.bitmap.pixels, region.width, region.height,
            position.first, 1);
        }
      }
      cue.position = static_cast<float>(pageRegion.x) / displayWidth;
      cue.line = static_cast<float>(pageRegion.y) / displayHeight;
      cue.size = static_cast<float>(region.width) / displayWidth;
      cue.bitmapHeight = static_cast<float>(region.height) / displayHeight;
      cue.positionAnchor = AVCue::AnchorType::kStart;
      cue.lineAnchor = AVCue::AnchorType::kStart;
      result.cues.push_back(std::move(cue));
    }
    if (!result.cues.empty()) results.push_back(std::move(result));
    page_.clear();
  }

  Display display_;
  std::vector<PageRegion> page_;
  std::map<int, Region> regions_;
  std::map<int, Object> objects_;
  std::map<int, Clut> cluts_;
  int subtitlePageId_ = -1;
};

AVDvbSubParser::AVDvbSubParser() : mImpl(new Impl()) {
}

AVDvbSubParser::~AVDvbSubParser() = default;

bool AVDvbSubParser::parse(const uint8_t* data, size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) return false;
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVDvbSubParser::reset() {
  mImpl->reset();
}

} // namespace coral
