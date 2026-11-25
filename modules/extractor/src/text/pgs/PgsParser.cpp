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

#include <extractor/text/pgs/PgsParser.h>

#include "text/SubtitleTextUtil.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace coral {
namespace {

constexpr uint8_t kSegmentPds = 0x14;
constexpr uint8_t kSegmentOds = 0x15;
constexpr uint8_t kSegmentPcs = 0x16;
constexpr uint8_t kSegmentWds = 0x17;
constexpr uint8_t kSegmentEnd = 0x80;

uint16_t readU16(const uint8_t* data, size_t size, size_t offset) {
  if (offset + 2 > size) return 0;
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

uint32_t readU32(const uint8_t* data, size_t size, size_t offset) {
  if (offset + 4 > size) return 0;
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

int64_t ptsToUs(const uint32_t pts) {
  return static_cast<int64_t>(pts) * 1000000 / 90000;
}

uint8_t clampByte(const float value) {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

struct Object final {
  int width = 0;
  int height = 0;
  /** The RLE payload; it is decoded at composition time so a later PDS update applies to it. */
  std::vector<uint8_t> rle;
};

/** An object whose ODS payload is split over several ODS segments. */
struct PartialObject final {
  uint16_t id = 0;
  uint8_t version = 0;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rle;
};

struct CompositionObject final {
  uint16_t id = 0;
  uint8_t windowId = 0;
  int x = 0;
  int y = 0;
  bool cropped = false;
  int cropX = 0;
  int cropY = 0;
  int cropWidth = 0;
  int cropHeight = 0;
};

/** One window of the presentation, in plane coordinates. */
struct Window final {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct Composition final {
  int width = 0;
  int height = 0;
  uint8_t paletteId = 0;
  std::vector<CompositionObject> objects;
};

struct Palette final {
  uint8_t version = 0;
  std::array<uint32_t, 256> entries{};
};

} // namespace

class AVPgsParser::Impl final {
public:
  void feed(const uint8_t* data, size_t size, std::vector<AVCuesWithTiming>& results) {
    std::vector<uint8_t> inflated;
    if (coral::text::maybeInflate(data, size, inflated)) {
      data = inflated.data();
      size = inflated.size();
    }
    size_t position = 0;
    while (position + 3 <= size) {
      const bool hasMagic = position + 13 <= size && data[position] == 0x50 &&
                            data[position + 1] == 0x47;
      // A standalone .sup starts each segment with the magic and PTS/DTS; Matroska stores the
      // segment type and size without the magic. Support both layouts.
      const size_t headerSize = hasMagic ? 13 : 3;
      const uint32_t pts = hasMagic ? readU32(data, size, position + 2) : 0;
      const uint8_t type = hasMagic ? data[position + 10] : data[position];
      const uint16_t payloadSize = hasMagic ? readU16(data, size, position + 11)
                                            : readU16(data, size, position + 1);
      const size_t payload = position + headerSize;
      if (payload + payloadSize > size) break;
      switch (type) {
        case kSegmentPcs:
          if (composition_.objects.empty() == false) emit(results);
          parsePcs(data + payload, payloadSize, pts, hasMagic);
          break;
        case kSegmentPds:
          parsePds(data + payload, payloadSize);
          break;
        case kSegmentOds:
          parseOds(data + payload, payloadSize);
          break;
        case kSegmentWds:
          parseWds(data + payload, payloadSize);
          break;
        case kSegmentEnd:
          emit(results);
          position = payload + payloadSize;
          continue;
        default:
          break;
      }
      position = payload + payloadSize;
    }
    // A sample may end without an explicit END segment.
    if (position >= size && composition_.objects.empty() == false) emit(results);
  }

  void reset() {
    palettes_.clear();
    objects_.clear();
    partialObjects_.clear();
    windows_.clear();
    composition_ = Composition{};
    lastCompositionObjects_.clear();
    lastWindows_.clear();
    compositionPtsUs_ = Definitions::kTimeUnset;
  }

private:
  void parseWds(const uint8_t* data, const size_t size) {
    if (size < 1) return;
    const uint8_t count = data[0];
    size_t position = 1;
    for (uint8_t i = 0; i < count && position + 9 <= size; ++i) {
      const uint8_t id = data[position];
      Window window;
      window.x = readU16(data, size, position + 1);
      window.y = readU16(data, size, position + 3);
      window.width = readU16(data, size, position + 5);
      window.height = readU16(data, size, position + 7);
      windows_[id] = window;
      position += 9;
    }
  }
  void parsePds(const uint8_t* data, const size_t size) {
    if (size < 2) return;
    // palette id = data[0], version = data[1]. A PDS may carry only the entries that changed, so
    // the existing palette is updated in place rather than replaced.
    const uint8_t paletteId = data[0];
    const uint8_t version = data[1];
    Palette& palette = palettes_[paletteId];
    palette.version = version;
    size_t position = 2;
    while (position + 5 <= size) {
      const uint8_t index = data[position];
      const float y = data[position + 1];
      const float cr = data[position + 2];
      const float cb = data[position + 3];
      const uint8_t alpha = data[position + 4];
      const float r = y + 1.402f * (cr - 128.0f);
      const float g = y - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
      const float b = y + 1.772f * (cb - 128.0f);
      palette.entries[index] = (static_cast<uint32_t>(alpha) << 24) |
                               (static_cast<uint32_t>(clampByte(r)) << 16) |
                               (static_cast<uint32_t>(clampByte(g)) << 8) |
                               clampByte(b);
      position += 5;
    }
  }

  static bool decodeRle(const uint8_t* data, size_t size, int width, int height,
      const std::array<uint32_t, 256>& palette, std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    rgba.assign(pixelCount * 4, 0);
    size_t read = 0;
    size_t write = 0;
    while (read < size && write < pixelCount) {
      const uint8_t flags = data[read++];
      size_t run = 0;
      uint8_t color = 0;
      if ((flags & 0xC0) == 0) {
        run = flags & 0x3F;
        if (read < size) color = data[read++];
      } else if ((flags & 0xC0) == 0x40) {
        if (read >= size) break;
        run = ((flags & 0x3F) << 8) | data[read++];
        if (read < size) color = data[read++];
      } else if ((flags & 0xC0) == 0x80) {
        if (read >= size) break;
        run = ((flags & 0x3F) << 8) | data[read++];
        if (read < size) color = data[read++];
      } else {
        run = 1;
        color = flags & 0x3F;
      }
      if (run == 0) continue;
      const uint32_t argb = palette[color];
      const uint8_t alpha = static_cast<uint8_t>((argb >> 24) & 0xFF);
      const uint8_t red = static_cast<uint8_t>((argb >> 16) & 0xFF);
      const uint8_t green = static_cast<uint8_t>((argb >> 8) & 0xFF);
      const uint8_t blue = static_cast<uint8_t>(argb & 0xFF);
      for (size_t i = 0; i < run && write < pixelCount; ++i, ++write) {
        rgba[write * 4 + 0] = red;
        rgba[write * 4 + 1] = green;
        rgba[write * 4 + 2] = blue;
        rgba[write * 4 + 3] = alpha;
      }
    }
    return write == pixelCount;
  }

  void parseOds(const uint8_t* data, const size_t size) {
    if (size < 11) return;
    const uint16_t id = readU16(data, size, 0);
    const uint8_t version = data[2];
    const uint8_t flags = data[3];
    const size_t dataSize = (static_cast<size_t>(data[4]) << 16) |
                            (static_cast<size_t>(data[5]) << 8) | data[6];
    if (dataSize > size - 11) return;
    const bool first = (flags & 0x80) != 0;
    const bool last = (flags & 0x40) != 0;

    if (!first && !last) {
      // A complete object in one segment, as most muxers produce.
      Object object;
      object.width = readU16(data, size, 7);
      object.height = readU16(data, size, 9);
      object.rle.assign(data + 11, data + 11 + dataSize);
      partialObjects_.erase(id);
      objects_[id] = std::move(object);
      return;
    }

    auto partial = partialObjects_.find(id);
    if (first) {
      PartialObject object;
      object.id = id;
      object.version = version;
      object.width = readU16(data, size, 7);
      object.height = readU16(data, size, 9);
      object.rle.assign(data + 11, data + 11 + dataSize);
      partialObjects_[id] = std::move(object);
      partial = partialObjects_.find(id);
    } else {
      if (partial == partialObjects_.end() || partial->second.version != version) {
        // The first segment was lost or belongs to another version; do not append garbage.
        return;
      }
      partial->second.rle.insert(partial->second.rle.end(), data + 11, data + 11 + dataSize);
    }

    if (last && partial != partialObjects_.end()) {
      Object object;
      object.width = partial->second.width;
      object.height = partial->second.height;
      object.rle = std::move(partial->second.rle);
      partialObjects_.erase(partial);
      if (object.width > 0 && object.height > 0 && !object.rle.empty()) {
        objects_[id] = std::move(object);
      }
    }
  }

  void parsePcs(const uint8_t* data, const size_t size, const uint32_t pts, const bool hasPts) {
    if (size < 11) return;
    const uint8_t paletteUpdateFlag = data[8];
    composition_ = Composition{};
    composition_.width = readU16(data, size, 0);
    composition_.height = readU16(data, size, 2);
    composition_.paletteId = data[9];
    const uint8_t objectCount = data[10];
    // A palette-only display set has no object records: it redraws the objects of the previous
    // composition with the updated palette. A zero-object set without the flag is a clear.
    if (objectCount == 0 && (paletteUpdateFlag & 0x80) != 0) {
      // A palette-only set has no object records and usually no WDS either; reuse both the object
      // list and the windows of the previous composition so cropped/positioned objects redraw in
      // the same place.
      composition_.objects = lastCompositionObjects_;
      windows_ = lastWindows_;
    }
    size_t position = 11;
    for (uint8_t i = 0; i < objectCount && position + 8 <= size; ++i) {
      CompositionObject object;
      object.id = readU16(data, size, position);
      object.windowId = data[position + 2];
      const uint8_t flags = data[position + 3];
      object.x = readU16(data, size, position + 4);
      object.y = readU16(data, size, position + 6);
      position += 8;
      if ((flags & 0x80) != 0 && position + 8 <= size) {
        object.cropped = true;
        object.cropX = readU16(data, size, position);
        object.cropY = readU16(data, size, position + 2);
        object.cropWidth = readU16(data, size, position + 4);
        object.cropHeight = readU16(data, size, position + 6);
        position += 8;
      }
      composition_.objects.push_back(object);
    }
    compositionPtsUs_ = hasPts ? ptsToUs(pts) : Definitions::kTimeUnset;
  }

  void emit(std::vector<AVCuesWithTiming>& results) {
    if (composition_.objects.empty()) {
      resetComposition();
      return;
    }
    struct RenderObject {
      const CompositionObject* composition;
      const Object* source;
      std::vector<uint8_t> rgba;
      int sourceX;
      int sourceY;
      int width;
      int height;
    };
    std::vector<RenderObject> renderObjects;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
    for (const auto& object : composition_.objects) {
      const auto found = objects_.find(object.id);
      if (found == objects_.end()) continue;
      Object& source = found->second;
      if (source.width <= 0 || source.height <= 0 || source.rle.empty()) continue;
      int sourceX = 0;
      int sourceY = 0;
      int width = source.width;
      int height = source.height;
      if (object.cropped) {
        sourceX = object.cropX;
        sourceY = object.cropY;
        width = object.cropWidth;
        height = object.cropHeight;
      }
      if (width <= 0 || height <= 0 || sourceX + width > source.width ||
          sourceY + height > source.height) {
        continue;
      }
      RenderObject render;
      render.composition = &object;
      render.source = &source;
      render.sourceX = sourceX;
      render.sourceY = sourceY;
      render.width = width;
      render.height = height;
      static const std::array<uint32_t, 256> kEmptyPalette{};
      const auto palette = palettes_.find(composition_.paletteId);
      const auto& entries = palette != palettes_.end() ? palette->second.entries : kEmptyPalette;
      if (!decodeRle(source.rle.data(), source.rle.size(), source.width, source.height, entries,
            render.rgba)) {
        continue;
      }
      const int originX = objectOriginX(object);
      const int originY = objectOriginY(object);
      // A window defines the cue's extent even when the objects in it are smaller; the pixels
      // outside the objects stay transparent. The drawing loop still clips each object to the
      // window rectangle.
      if (const auto window = windows_.find(object.windowId); window != windows_.end()) {
        minX = std::min(minX, window->second.x);
        minY = std::min(minY, window->second.y);
        maxX = std::max(maxX, window->second.x + window->second.width);
        maxY = std::max(maxY, window->second.y + window->second.height);
      } else {
        minX = std::min(minX, originX);
        minY = std::min(minY, originY);
        maxX = std::max(maxX, originX + width);
        maxY = std::max(maxY, originY + height);
      }
      renderObjects.push_back(std::move(render));
    }
    if (renderObjects.empty() || minX == std::numeric_limits<int>::max() || maxX <= minX ||
        maxY <= minY) {
      resetComposition();
      return;
    }
    const int width = maxX - minX;
    const int height = maxY - minY;
    AVCue cue;
    cue.bitmap.width = width;
    cue.bitmap.height = height;
    cue.bitmap.format = PixelFormat::kFormatRGBA;
    cue.bitmap.rowBytes = static_cast<uint32_t>(width) * 4;
    cue.bitmap.premultiplied = false;
    cue.bitmap.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    if (composition_.width > 0 && composition_.height > 0) {
      // Position the cropped union of the windows on the video plane; a window's objects are
      // relative to the window, and the window is relative to the plane.
      cue.position = static_cast<float>(minX) / static_cast<float>(composition_.width);
      cue.line = static_cast<float>(minY) / static_cast<float>(composition_.height);
      cue.size = static_cast<float>(width) / static_cast<float>(composition_.width);
      cue.bitmapHeight = static_cast<float>(height) / static_cast<float>(composition_.height);
      cue.positionAnchor = AVCue::AnchorType::kStart;
      cue.lineAnchor = AVCue::AnchorType::kStart;
      cue.lineType = AVCue::LineType::kFraction;
    }
    for (const auto& render : renderObjects) {
      const auto& object = *render.composition;
      const int originX = objectOriginX(object);
      const int originY = objectOriginY(object);
      const auto window = windows_.find(object.windowId);
      for (int y = 0; y < render.height; ++y) {
        const int globalY = originY + y;
        if (window != windows_.end() &&
            (globalY < window->second.y || globalY >= window->second.y + window->second.height)) {
          continue;
        }
        for (int x = 0; x < render.width; ++x) {
          const int globalX = originX + x;
          if (window != windows_.end() &&
              (globalX < window->second.x || globalX >= window->second.x + window->second.width)) {
            continue;
          }
          const int targetX = globalX - minX;
          const int targetY = globalY - minY;
          if (targetX < 0 || targetX >= width || targetY < 0 || targetY >= height) continue;
          const size_t sourceOffset =
            (static_cast<size_t>(render.sourceY + y) * render.source->width +
              static_cast<size_t>(render.sourceX + x)) * 4;
          const size_t targetOffset = (static_cast<size_t>(targetY) * width + targetX) * 4;
          const uint8_t sourceAlpha = render.rgba[sourceOffset + 3];
          if (sourceAlpha == 0) {
            continue;
          }
          if (sourceAlpha == 255) {
            for (size_t i = 0; i < 4; ++i) {
              cue.bitmap.pixels[targetOffset + i] = render.rgba[sourceOffset + i];
            }
            continue;
          }
          // The cue bitmap is unpremultiplied RGBA. Composite the next PGS object over the pixels
          // already written by earlier objects, preserving the PCS order.
          const uint8_t destinationAlpha = cue.bitmap.pixels[targetOffset + 3];
          const uint32_t outAlpha = sourceAlpha +
            (static_cast<uint32_t>(destinationAlpha) * (255u - sourceAlpha) + 127u) / 255u;
          if (outAlpha == 0) {
            continue;
          }
          for (size_t channel = 0; channel < 3; ++channel) {
            const uint64_t numerator =
              static_cast<uint64_t>(render.rgba[sourceOffset + channel]) * sourceAlpha * 255u +
              static_cast<uint64_t>(cue.bitmap.pixels[targetOffset + channel]) *
                destinationAlpha * (255u - sourceAlpha);
            cue.bitmap.pixels[targetOffset + channel] =
              static_cast<uint8_t>(numerator / (outAlpha * 255u));
          }
          cue.bitmap.pixels[targetOffset + 3] = static_cast<uint8_t>(outAlpha);
        }
      }
    }
    AVCuesWithTiming result;
    result.startTimeUs = compositionPtsUs_;
    result.durationUs = Definitions::kTimeUnset;
    result.cues.push_back(std::move(cue));
    results.push_back(std::move(result));
    lastCompositionObjects_ = composition_.objects;
    lastWindows_ = windows_;
    resetComposition();
  }

  int objectOriginX(const CompositionObject& object) const {
    const auto window = windows_.find(object.windowId);
    return object.x + (window == windows_.end() ? 0 : window->second.x);
  }

  int objectOriginY(const CompositionObject& object) const {
    const auto window = windows_.find(object.windowId);
    return object.y + (window == windows_.end() ? 0 : window->second.y);
  }

  void resetComposition() {
    composition_ = Composition{};
    windows_.clear();
    compositionPtsUs_ = Definitions::kTimeUnset;
  }

  std::map<uint8_t, Palette> palettes_;
  std::map<uint16_t, Object> objects_;
  std::map<uint16_t, PartialObject> partialObjects_;
  std::map<uint8_t, Window> windows_;
  Composition composition_;
  std::vector<CompositionObject> lastCompositionObjects_;
  std::map<uint8_t, Window> lastWindows_;
  int64_t compositionPtsUs_ = Definitions::kTimeUnset;
};

AVPgsParser::AVPgsParser() : mImpl(new Impl()) {
}

AVPgsParser::~AVPgsParser() = default;

bool AVPgsParser::parse(const uint8_t* data, size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) return false;
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVPgsParser::reset() {
  mImpl->reset();
}

} // namespace coral
