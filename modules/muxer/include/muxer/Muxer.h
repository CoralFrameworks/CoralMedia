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

#ifndef CORAL_MUXER_MUXER_H
#define CORAL_MUXER_MUXER_H

#include "muxer/MuxerTypes.h"

#include <media/Metadata.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace coral {
namespace muxer {

/**
 * Writes encoded tracks into a container.
 *
 * The muxer is a synchronous, caller-thread object. It does not own a thread and performs no
 * buffering beyond what interleaving and the container layout require; a caller that reads samples
 * from decoders or encoders keeps doing that on its own event thread.
 *
 * The caller writes each track in decode order. `SampleInfo::presentationTimeUs` is the
 * presentation time; for a track with B-frames the decode order and the presentation order differ,
 * and the muxer derives the container's `stts`/`ctts` tables from the write order and the
 * presentation timestamps.
 *
 * All tracks must be added before the first sample is written. A muxer is not reusable once
 * `finish()` or `cancel()` has been called.
 */
class Muxer {
public:
  Muxer() = default;
  virtual ~Muxer() = default;

  Muxer(const Muxer&) = delete;
  Muxer& operator=(const Muxer&) = delete;

  /**
   * Adds an encoded track.
   *
   * @param format The codec format, including codec-specific data (`csd-0`, ...) and dimensions.
   * @param options Track settings that are not part of the codec format.
   * @return The track id used by `writeSample`, or -1 when the track cannot be represented by this
   *         container. A derived track (e.g. captions that an extractor derives from the SEI of a
   *         video track) is rejected because its samples are already carried by its source track,
   *         and so is a codec the container does not write. A rejected track does not fail the
   *         muxer: the caller skips it and keeps adding the other tracks, and the error is then
   *         available from `getError()`.
   */
  virtual int addTrack(const std::shared_ptr<MediaFormat>& format,
    const TrackOptions& options = {}) = 0;

  /** Writes one encoded sample. */
  virtual bool writeSample(int trackId, const uint8_t* data, size_t size,
    const SampleInfo& info) = 0;

  /** Writes one encoded sample carried by a Coral packet buffer. */
  virtual bool writeSample(int trackId, const std::shared_ptr<AVPacketBuffer>& packet);

  /** Adds typed file-level metadata. May be called before the first sample. */
  virtual bool addMetadata(const std::shared_ptr<Metadata>& metadata) = 0;

  /** Completes the container. The muxer cannot be used after this call. */
  virtual bool finish() = 0;

  /** Abandons the container and releases the output. */
  virtual void cancel() = 0;

  [[nodiscard]] virtual Status getStatus() const noexcept = 0;

  [[nodiscard]] virtual Error getError() const = 0;

  [[nodiscard]] virtual Container getContainer() const noexcept = 0;
};

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MUXER_H
