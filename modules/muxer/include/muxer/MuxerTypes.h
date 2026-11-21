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

#ifndef CORAL_MUXER_MUXER_TYPES_H
#define CORAL_MUXER_MUXER_TYPES_H

#include <media/Format.h>
#include <media/PacketBuffer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {

/** The container a muxer writes. */
enum class Container : uint8_t {
  kMp4 = 0,
  kFragmentedMp4 = 1,
  kWebM = 2,
  kOgg = 3,
  kWav = 4,
  kAdts = 5,
};

enum class Status : uint8_t {
  kIdle = 0,
  kWriting = 1,
  kCompleted = 2,
  kFailed = 3,
  kCancelled = 4,
};

enum class SampleFlag : uint32_t {
  kNone = 0,
  kKeyFrame = 1u << 0,
  kCodecConfig = 1u << 1,
  kDiscard = 1u << 2,
};

using SampleFlags = uint32_t;

[[nodiscard]] constexpr bool hasSampleFlag(const SampleFlags flags, const SampleFlag flag) noexcept {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

/** How a sample is placed on the track's timeline. */
struct SampleInfo {
  /** Presentation time on the track timeline, in microseconds. */
  int64_t presentationTimeUs = 0;
  /**
   * The duration of the sample, in microseconds, or zero when the muxer should derive it from the
   * next sample. A caller that writes in decode order can leave this unset for every sample but the
   * last one.
   */
  int64_t durationUs = 0;
  /** Key-frame/codec-config flags carried by the encoded sample. */
  SampleFlags flags = 0;
};

/** Per-track settings that are not part of the codec format. */
struct TrackOptions {
  /** A caller-chosen id, or -1 to let the muxer allocate one. */
  int trackId = -1;
  /** ISO-639-2/T language code, e.g. `eng`. Empty means `und`. */
  std::string language;
  bool enabled = true;
  /** An auxiliary track (depth, alpha, metadata) rather than a primary A/V track. */
  bool auxiliary = false;
  /** MP4-AT auxiliary track type: 0 original, 1 depth linear, 2 depth inverse, 3 depth metadata. */
  int auxiliaryTrackType = 0;
  int alternateGroup = 0;
};

/**
 * File-level options. The defaults are the safe ones for a finished local file; a fragmenting or
 * streaming configuration sets `fragmentDurationUs` and `fastStart`.
 */
struct MuxerOptions {
  /**
   * Write the `moov` box before `mdat` so a player can start before the file is complete. Seekable
   * outputs relocate the buffered `mdat`; non-seekable outputs spool it to a temporary file.
   */
  bool fastStart = false;
  /** Non-zero writes a fragmented MP4 (one fragment per this duration). */
  int64_t fragmentDurationUs = 0;
  /** The interleaving window for unfragmented files. */
  int64_t interleaveDurationUs = 1'000'000;
  /** Keep negative source timestamps in an edit list instead of shifting every track. */
  bool writeNegativeTimestampsInEditList = true;
  int64_t movieTimescale = 1'000;
  /** Write auxiliary tracks into an MP4-AT `axte` box instead of the primary movie. */
  bool auxiliaryTracksInAxte = false;
};

struct Error {
  int code = 0;
  std::string message;
};

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MUXER_TYPES_H
