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

#include <extractor/text/webvtt/Mp4WebvttParser.h>

#include "text/SubtitleTextUtil.h"
#include "text/webvtt/WebvttCueParser.h"

#include <media/Definitions.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr uint32_t kBoxHeaderSize = 8;
constexpr uint32_t kTypePayl = 0x7061796C;   // 'payl'
constexpr uint32_t kTypeSttg = 0x73747467;   // 'sttg'
constexpr uint32_t kTypeVttc = 0x76747463;   // 'vttc'

uint32_t readUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

/**
 * Parses one `vttc` box.
 *
 * @param payload The bytes of the box content, i.e. without its header.
 * @param size The number of bytes of the content.
 * @param[out] cue Set to the cue the box describes.
 *
 * @return {@code false} when the content is malformed.
 */
bool parseVttCueBox(const uint8_t* payload, const size_t size, AVCue& cue) {
  std::string settings;
  std::string text;
  size_t position = 0;
  while (position < size) {
    if (size - position < kBoxHeaderSize) {
      return false;
    }
    const uint32_t boxSize = readUInt32(payload + position);
    const uint32_t boxType = readUInt32(payload + position + 4);
    if (boxSize < kBoxHeaderSize || boxSize > size - position) {
      return false;
    }
    const size_t payloadSize = boxSize - kBoxHeaderSize;
    const char* boxPayload = reinterpret_cast<const char*>(payload + position + kBoxHeaderSize);
    if (boxType == kTypeSttg) {
      settings.assign(boxPayload, payloadSize);
    } else if (boxType == kTypePayl) {
      text.assign(boxPayload, payloadSize);
    }
    position += boxSize;
  }

  text::webvtt::applyCueDefaults(cue);
  text::webvtt::applyCueSettings(settings, cue);
  text::webvtt::parseCueText(text::trim(text), cue);
  text::webvtt::finalizeCue(cue);
  return true;
}

} // namespace

bool AVMp4WebvttParser::parse(const uint8_t* data, const size_t size,
                              const OutputOptions& options, const CueConsumer& output) {
  if (output == nullptr) {
    return false;
  }
  AVCuesWithTiming result;
  result.startTimeUs = Definitions::kTimeUnset;
  result.durationUs = Definitions::kTimeUnset;

  size_t position = 0;
  if (data != nullptr) {
    while (position < size) {
      if (size - position < kBoxHeaderSize) {
        return false;
      }
      const uint32_t boxSize = readUInt32(data + position);
      const uint32_t boxType = readUInt32(data + position + 4);
      if (boxSize < kBoxHeaderSize || boxSize > size - position) {
        return false;
      }
      if (boxType == kTypeVttc) {
        AVCue cue;
        if (!parseVttCueBox(data + position + kBoxHeaderSize, boxSize - kBoxHeaderSize, cue)) {
          return false;
        }
        if (!cue.isEmpty()) {
          result.cues.push_back(std::move(cue));
        }
      }
      // The peers of a cue box (and `vtte`, the box of an empty cue) are skipped: a sample that
      // holds no cue still clears the screen of a track that replaces its cues.
      position += boxSize;
    }
  }

  // One sample is one cue set, and its time is the time of the sample: the options are irrelevant
  // here because there is exactly one result to emit.
  (void)options;
  output(result);
  return true;
}

void AVMp4WebvttParser::reset() {
  // The parser holds no state across samples.
}

} // namespace coral
