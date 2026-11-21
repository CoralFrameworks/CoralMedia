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

#ifndef CORAL_MUXER_MP4_SAMPLE_TABLE_H
#define CORAL_MUXER_MP4_SAMPLE_TABLE_H

#include "BoxWriter.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/** One sample of a track, in decode order. */
struct SampleRecord {
  uint32_t size = 0;
  int64_t presentationTimeUs = 0;
  int64_t durationUs = 0;
  uint32_t flags = 0;
};

/** A contiguous run of samples of one track in the media data. */
struct ChunkRecord {
  int64_t offset = 0;
  uint32_t sampleCount = 0;
};

/**
 * The per-track sample metadata and the MP4 sample table boxes derived from it.
 *
 * Samples are kept in write (decode) order. Durations may be given per sample or derived from the
 * presentation-time order; the derived tables always use the decode order for `stts`/`ctts` and the
 * write order for `stss`/`stsz`.
 */
class SampleTable final {
public:
  static constexpr int64_t kUnsetPresentationTime = std::numeric_limits<int64_t>::min();

  explicit SampleTable(int32_t timescale = 90000);

  void setTimescale(int32_t timescale);

  [[nodiscard]] int32_t timescale() const { return mTimescale; }

  void addSample(uint32_t size, int64_t presentationTimeUs, int64_t durationUs, uint32_t flags);

  void addChunk(int64_t offset);

  void incrementLastChunkSampleCount();

  [[nodiscard]] bool empty() const { return mSamples.empty(); }

  [[nodiscard]] size_t size() const { return mSamples.size(); }

  [[nodiscard]] const std::vector<SampleRecord>& samples() const { return mSamples; }

  [[nodiscard]] const std::vector<ChunkRecord>& chunks() const { return mChunks; }

  /**
   * Derives the duration of every sample that does not have an explicit one.
   *
   * @param defaultDurationUs The fallback duration, e.g. one audio frame or one video frame, or 0.
   * @param endPresentationTimeUs The presentation time that follows the last sample, or
   *                              `kUnsetPresentationTime` when unknown.
   */
  void finalize(int64_t defaultDurationUs, int64_t endPresentationTimeUs = kUnsetPresentationTime);

  [[nodiscard]] int64_t firstPresentationTimeUs() const;
  [[nodiscard]] int64_t minimumPresentationTimeUs() const;
  [[nodiscard]] int64_t maximumPresentationTimeUs() const;

  [[nodiscard]] int64_t scaleUs(int64_t timeUs) const;

  [[nodiscard]] int64_t sampleDurationUs(size_t index) const;
  [[nodiscard]] int64_t sampleDurationTicks(size_t index) const;
  [[nodiscard]] int64_t durationTicks() const;
  [[nodiscard]] int64_t durationUs() const;

  [[nodiscard]] bool allKeyFrames() const;
  [[nodiscard]] std::vector<uint32_t> syncSampleNumbers() const;

  /** Whether a `ctts` box is required for this track. */
  [[nodiscard]] bool hasCompositionOffsets() const;

  /** Composition offsets in track ticks, relative to the first written sample. */
  [[nodiscard]] std::vector<int32_t> compositionOffsetsTicks() const;

  void shiftChunkOffsets(int64_t delta);

  /** Moves every presentation time by @p delta microseconds, e.g. to remove a negative start. */
  void shiftPresentationTimes(int64_t delta);

  [[nodiscard]] bool needs64BitChunkOffsets() const;

  void writeStts(BoxWriter& writer) const;
  void writeCtts(BoxWriter& writer) const;
  void writeStss(BoxWriter& writer) const;
  void writeStsz(BoxWriter& writer) const;
  void writeStsc(BoxWriter& writer) const;
  void writeChunkOffsets(BoxWriter& writer, bool force64Bit) const;

private:
  int32_t mTimescale;
  std::vector<SampleRecord> mSamples;
  std::vector<ChunkRecord> mChunks;
};

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_SAMPLE_TABLE_H
