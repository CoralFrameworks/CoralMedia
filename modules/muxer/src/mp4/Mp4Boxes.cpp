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

#include "Mp4Boxes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

constexpr uint32_t kMatrixOne = 0x00010000;

std::string normalizeLanguage(const std::string& language) {
  std::string code;
  code.reserve(3);
  for (const char value: language) {
    if (code.size() == 3) {
      break;
    }
    const auto character = static_cast<unsigned char>(value);
    if (std::isalpha(character) == 0) {
      return "und";
    }
    code.push_back(static_cast<char>(std::tolower(character)));
  }
  return code.size() == 3 ? code : "und";
}

} // namespace

int64_t scaleUsToTicks(const int64_t timeUs, const int64_t timescale) {
  if (timescale <= 0) {
    return timeUs;
  }
  return static_cast<int64_t>(std::llround(static_cast<long double>(timeUs) * static_cast<long double>(timescale) /
                                           static_cast<long double>(kMicrosPerSecond)));
}

int64_t scaleTicksToUs(const int64_t ticks, const int64_t timescale) {
  if (timescale <= 0) {
    return ticks;
  }
  return static_cast<int64_t>(
          std::llround(static_cast<long double>(ticks) * static_cast<long double>(kMicrosPerSecond) /
                       static_cast<long double>(timescale)));
}

uint32_t clampToU32(const int64_t value) {
  if (value <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(std::min<int64_t>(value, static_cast<int64_t>(std::numeric_limits<uint32_t>::max())));
}

uint64_t clampToU64(const int64_t value) { return value <= 0 ? 0 : static_cast<uint64_t>(value); }

uint16_t packLanguage(const std::string& language) {
  const std::string code = normalizeLanguage(language);
  return static_cast<uint16_t>(
          ((code[0] - 0x60) & 0x1F) << 10 | ((code[1] - 0x60) & 0x1F) << 5 | ((code[2] - 0x60) & 0x1F));
}

void writeMatrix(BoxWriter& writer, const int rotation) {
  switch (((rotation % 360) + 360) % 360) {
    case 90:
      writer.i32(0);
      writer.i32(kMatrixOne);
      writer.i32(0);
      writer.i32(-static_cast<int32_t>(kMatrixOne));
      writer.i32(0);
      writer.i32(0);
      break;
    case 180:
      writer.i32(-static_cast<int32_t>(kMatrixOne));
      writer.i32(0);
      writer.i32(0);
      writer.i32(0);
      writer.i32(-static_cast<int32_t>(kMatrixOne));
      writer.i32(0);
      break;
    case 270:
      writer.i32(0);
      writer.i32(-static_cast<int32_t>(kMatrixOne));
      writer.i32(0);
      writer.i32(kMatrixOne);
      writer.i32(0);
      writer.i32(0);
      break;
    default:
      writer.i32(kMatrixOne);
      writer.i32(0);
      writer.i32(0);
      writer.i32(0);
      writer.i32(kMatrixOne);
      writer.i32(0);
      break;
  }
  writer.i32(0);
  writer.i32(0);
  writer.i32(0x40000000);
}

void writeFtyp(BoxWriter& writer) {
  const size_t box = writer.beginBox("ftyp");
  writer.fourcc("isom");
  writer.u32(0x00000200);
  writer.fourcc("isom");
  writer.fourcc("iso2");
  writer.fourcc("avc1");
  writer.fourcc("mp41");
  writer.endBox(box);
}

void writeMvhd(BoxWriter& writer, const int64_t durationUs, const int64_t movieTimescale, const int nextTrackId,
        const int64_t creationTime, const int64_t modificationTime) {
  const int64_t durationTicks = std::max<int64_t>(0, scaleUsToTicks(durationUs, movieTimescale));
  const bool version1 = durationTicks > std::numeric_limits<uint32_t>::max();

  const size_t mvhd = writer.beginFullBox("mvhd", version1 ? 1 : 0, 0);
  if (version1) {
    writer.u64(clampToU64(creationTime));
    writer.u64(clampToU64(modificationTime));
    writer.u32(clampToU32(movieTimescale));
    writer.u64(static_cast<uint64_t>(durationTicks));
  } else {
    writer.u32(clampToU32(creationTime));
    writer.u32(clampToU32(modificationTime));
    writer.u32(clampToU32(movieTimescale));
    writer.u32(clampToU32(durationTicks));
  }
  writer.u32(0x00010000); // rate 1.0
  writer.u16(0x0100);     // volume 1.0
  writer.u16(0);
  writer.u32(0);
  writer.u32(0);
  writeMatrix(writer, 0);
  writer.zeros(24);
  writer.u32(static_cast<uint32_t>(std::max(1, nextTrackId)));
  writer.endBox(mvhd);
}

void writeTkhd(BoxWriter& writer, const int trackId, const int64_t presentationDurationUs, const int64_t movieTimescale,
        const int64_t creationTime, const int64_t modificationTime, const int rotation, const int alternateGroup,
        const bool enabled, const bool audio, const int width, const int height) {
  const int64_t durationTicks = std::max<int64_t>(0, scaleUsToTicks(presentationDurationUs, movieTimescale));
  const bool version1 = durationTicks > std::numeric_limits<uint32_t>::max();
  const uint32_t flags = 0x2u | (enabled ? 0x1u : 0u);

  const size_t tkhd = writer.beginFullBox("tkhd", version1 ? 1 : 0, flags);
  if (version1) {
    writer.u64(clampToU64(creationTime));
    writer.u64(clampToU64(modificationTime));
    writer.u32(static_cast<uint32_t>(trackId));
    writer.u32(0);
    writer.u64(static_cast<uint64_t>(durationTicks));
  } else {
    writer.u32(clampToU32(creationTime));
    writer.u32(clampToU32(modificationTime));
    writer.u32(static_cast<uint32_t>(trackId));
    writer.u32(0);
    writer.u32(clampToU32(durationTicks));
  }
  writer.u32(0);
  writer.u32(0);
  writer.u16(0); // layer
  writer.u16(static_cast<uint16_t>(std::max(0, alternateGroup)));
  writer.u16(audio ? 0x0100 : 0); // volume
  writer.u16(0);
  writeMatrix(writer, rotation);
  writer.u32(audio ? 0 : static_cast<uint32_t>(std::max(0, width)) << 16);
  writer.u32(audio ? 0 : static_cast<uint32_t>(std::max(0, height)) << 16);
  writer.endBox(tkhd);
}

void writeMdhd(BoxWriter& writer, const int64_t durationTicks, const int64_t timescale, const int64_t creationTime,
        const int64_t modificationTime, const std::string& language) {
  const bool version1 = durationTicks > std::numeric_limits<uint32_t>::max();
  const size_t mdhd = writer.beginFullBox("mdhd", version1 ? 1 : 0, 0);
  if (version1) {
    writer.u64(clampToU64(creationTime));
    writer.u64(clampToU64(modificationTime));
    writer.u32(clampToU32(timescale));
    writer.u64(clampToU64(durationTicks));
  } else {
    writer.u32(clampToU32(creationTime));
    writer.u32(clampToU32(modificationTime));
    writer.u32(clampToU32(timescale));
    writer.u32(clampToU32(durationTicks));
  }
  writer.u16(packLanguage(language));
  writer.u16(0);
  writer.endBox(mdhd);
}

void writeHdlr(BoxWriter& writer, const char* handlerType, const char* name) {
  const size_t hdlr = writer.beginFullBox("hdlr", 0, 0);
  writer.u32(0);
  writer.fourcc(handlerType);
  writer.zeros(12);
  writer.string(name);
  writer.u8(0);
  writer.endBox(hdlr);
}

void writeDinf(BoxWriter& writer) {
  const size_t dinf = writer.beginBox("dinf");
  const size_t dref = writer.beginFullBox("dref", 0, 0);
  writer.u32(1);
  const size_t url = writer.beginFullBox("url ", 0, 1);
  writer.endBox(url);
  writer.endBox(dref);
  writer.endBox(dinf);
}

} // namespace mp4
} // namespace muxer
} // namespace coral
