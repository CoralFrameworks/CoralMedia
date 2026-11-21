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

#ifndef CORAL_MUXER_WEBM_WEBM_MUXER_H
#define CORAL_MUXER_WEBM_WEBM_MUXER_H

#include "MuxerOutput.h"
#include "muxer/Muxer.h"

#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace webm {

enum class WebmCodec : uint8_t {
  kUnknown = 0,
  kVp8,
  kVp9,
  kAv1,
  kOpus,
  kVorbis,
};

/** A WebM (Matroska subset) writer for VP8/VP9/AV1 video and Opus/Vorbis audio. */
class WebmMuxer final : public Muxer {
public:
  static std::unique_ptr<WebmMuxer> create(const std::string& url, const MuxerOptions& options,
    Error* error);

  ~WebmMuxer() override;

  int addTrack(const std::shared_ptr<MediaFormat>& format,
    const TrackOptions& options) override;

  bool writeSample(int trackId, const uint8_t* data, size_t size,
    const SampleInfo& info) override;

  bool addMetadata(const std::shared_ptr<Metadata>& metadata) override;

  bool finish() override;

  void cancel() override;

  [[nodiscard]] Status getStatus() const noexcept override { return mStatus; }

  [[nodiscard]] Error getError() const override { return mError; }

  [[nodiscard]] Container getContainer() const noexcept override {
    return Container::kWebM;
  }

private:
  struct PendingSample {
    std::vector<uint8_t> data;
    int64_t presentationTimeUs = 0;
    int64_t durationUs = 0;
    uint32_t flags = 0;
  };

  struct Track {
    std::shared_ptr<MediaFormat> format;
    TrackOptions options;
    int id = -1;
    int number = 0;
    WebmCodec codec = WebmCodec::kUnknown;
    std::vector<PendingSample> pending;
    int64_t lastPresentationTimeUs = 0;
    int64_t lastDurationUs = 0;
    bool hadKeyFrame = false;
  };

  WebmMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept;

  bool writeHeader();
  bool flushCluster();
  bool hasPendingSamples() const;
  bool hasVideoTrack() const;
  bool shouldFlushBefore(const Track& track, const SampleInfo& info, bool keyFrame) const;
  int64_t defaultDurationUs(const Track& track) const;

  bool fail(std::string message);
  bool reject(std::string message);
  bool writeBytes(const uint8_t* data, size_t size);
  [[nodiscard]] Track* findTrack(int trackId);
  [[nodiscard]] const Track* findTrack(int trackId) const;

  std::unique_ptr<MuxerOutput> mOutput;
  MuxerOptions mOptions;
  std::vector<Track> mTracks;
  Status mStatus = Status::kIdle;
  Error mError;
  int mNextTrackId = 1;
  int64_t mDurationPosition = -1;
  int64_t mClusterStartUs = 0;
  int64_t mLastEndUs = 0;
  bool mHeaderWritten = false;
  bool mHasSamples = false;
};

} // namespace webm
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_WEBM_WEBM_MUXER_H
