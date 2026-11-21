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

#ifndef CORAL_MUXER_MP4_FRAGMENTED_MUXER_H
#define CORAL_MUXER_MP4_FRAGMENTED_MUXER_H

#include <muxer/Muxer.h>

#include "Mp4Metadata.h"
#include "Mp4SampleEntry.h"
#include "MuxerOutput.h"

#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/**
 * A fragmented MP4 writer.
 *
 * The `moov` box with `mvex`/`trex` is written before the first fragment. Samples are buffered per
 * track and emitted as `moof` + `mdat` pairs when the fragment duration is reached; a video fragment
 * starts on a key frame. Every `traf` carries an absolute `tfdt` so fragment time is continuous.
 */
class FragmentedMp4Muxer final : public Muxer {
public:
  static std::unique_ptr<FragmentedMp4Muxer> create(const std::string& url, const MuxerOptions& options, Error* error);

  ~FragmentedMp4Muxer() override;

  int addTrack(const std::shared_ptr<MediaFormat>& format, const TrackOptions& options) override;

  bool writeSample(int trackId, const uint8_t* data, size_t size, const SampleInfo& info) override;

  bool addMetadata(const std::shared_ptr<Metadata>& metadata) override;

  bool finish() override;

  void cancel() override;

  [[nodiscard]] Status getStatus() const noexcept override { return mStatus; }

  [[nodiscard]] Error getError() const override { return mError; }

  [[nodiscard]] Container getContainer() const noexcept override { return Container::kFragmentedMp4; }

private:
  struct PendingSample {
    std::vector<uint8_t> data;
    int64_t presentationTimeUs = 0;
    int64_t durationUs = 0;
    uint32_t flags = 0;
  };

  struct Track {
    explicit Track(int32_t trackTimescale)
            : timescale(trackTimescale) {}

    std::shared_ptr<MediaFormat> format;
    TrackOptions options;
    int id = -1;
    int32_t timescale = 90000;
    Codec codec = Codec::kUnknown;
    std::vector<PendingSample> pending;
    int64_t pendingMinPts = 0;
    int64_t pendingMaxPts = 0;
    int64_t pendingBytes = 0;
    int64_t nextDecodeTimeTicks = 0;
    int64_t lastSampleDurationUs = 0;
    bool hadKeyFrame = false;
  };

  FragmentedMp4Muxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept;

  bool writeHeader();
  bool writeMoov(BoxWriter& writer);
  bool writeEmptyTrack(BoxWriter& writer, const Track& track);
  bool flushFragment();
  bool shouldFlushBefore(const Track& track, const SampleInfo& info, bool keyFrame) const;
  bool hasVideoTrack() const;

  bool fail(std::string message);
  bool reject(std::string message);
  bool writeBytes(const uint8_t* data, size_t size);
  [[nodiscard]] Track* findTrack(int trackId);
  [[nodiscard]] const Track* findTrack(int trackId) const;

  std::unique_ptr<MuxerOutput> mOutput;
  MuxerOptions mOptions;
  std::vector<Track> mTracks;
  Mp4Metadata mMetadata;
  Status mStatus = Status::kIdle;
  Error mError;
  int64_t mFragmentDurationUs = 2'000'000;
  int mSequenceNumber = 1;
  int mNextTrackId = 1;
  bool mHeaderWritten = false;
  bool mHasSamples = false;
};

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_FRAGMENTED_MUXER_H
