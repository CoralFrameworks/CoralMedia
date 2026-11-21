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

#ifndef CORAL_MUXER_MP4_SAMPLE_ENTRY_H
#define CORAL_MUXER_MP4_SAMPLE_ENTRY_H

#include <media/Format.h>

#include "BoxWriter.h"

#include <cstdint>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/** The codecs the MP4 writer knows how to describe. */
enum class Codec : uint8_t {
  kUnknown = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
  kVp9 = 4,
  kMpeg4 = 5,
  kAac = 6,
  kOpus = 7,
  kFlac = 8,
  kAc3 = 9,
  kEac3 = 10,
  kH263 = 11,
  kDolbyVision = 12,
  kRawAudio = 13,
  kCea608 = 14,
  kCea708 = 15,
  kTx3g = 16,
  kWvtt = 17,
  kStpp = 18,
};

[[nodiscard]] Codec codecFor(const MediaFormat& format);

[[nodiscard]] bool isAudioCodec(Codec codec);

[[nodiscard]] bool isVideoCodec(Codec codec);

/** Whether the codec is a text (subtitle) track, written with a `text` handler and `nmhd`. */
[[nodiscard]] bool isTextCodec(Codec codec);

/** Whether the codec is a closed caption track, written as a `c608`/`c708` sample entry. */
[[nodiscard]] bool isCaptionCodec(Codec codec);

/** Whether the sample entry needs codec-specific data from the format. */
[[nodiscard]] bool requiresCodecSpecificData(Codec codec);

/** The per-track timescale: the sample rate for audio, 90000 for video. */
[[nodiscard]] int32_t chooseTrackTimescale(Codec codec, const MediaFormat& format);

/** The fallback sample duration when neither a duration nor a next timestamp is available. */
[[nodiscard]] int64_t defaultSampleDurationUs(Codec codec, const MediaFormat& format);

[[nodiscard]] bool startsWithStartCode(const uint8_t* data, size_t size);

std::vector<std::vector<uint8_t>> splitNalUnits(const std::vector<uint8_t>& data);

std::vector<uint8_t> annexBToLengthPrefixed(const uint8_t* data, size_t size);

/** Wraps the cc_data of a caption sample in the `cdat` box of a `c608`/`c708` sample entry. */
std::vector<uint8_t> wrapCaptionSample(const uint8_t* data, size_t size);

/** Writes the `stsd` sample entry for a track. */
void writeSampleEntry(BoxWriter& writer, Codec codec, const MediaFormat& format);

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_SAMPLE_ENTRY_H
