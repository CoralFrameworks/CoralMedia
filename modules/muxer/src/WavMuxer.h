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

#ifndef CORAL_MUXER_WAV_MUXER_H
#define CORAL_MUXER_WAV_MUXER_H

#include "MuxerOutput.h"
#include "muxer/Muxer.h"

#include <memory>
#include <string>

namespace coral {
namespace muxer {

/** A RIFF/WAVE writer for 16/24/32-bit PCM and 32-bit float audio. */
class WavMuxer final : public Muxer {
public:
  static std::unique_ptr<WavMuxer> create(const std::string& url, const MuxerOptions& options,
    Error* error);

  ~WavMuxer() override;

  int addTrack(const std::shared_ptr<MediaFormat>& format,
    const TrackOptions& options) override;

  bool writeSample(int trackId, const uint8_t* data, size_t size,
    const SampleInfo& info) override;

  bool addMetadata(const std::shared_ptr<Metadata>& metadata) override;

  bool finish() override;

  void cancel() override;

  [[nodiscard]] Status getStatus() const noexcept override { return mStatus; }

  [[nodiscard]] Error getError() const override { return mError; }

  [[nodiscard]] Container getContainer() const noexcept override { return Container::kWav; }

private:
  WavMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept;

  bool writeHeader();
  bool patchSizes();
  bool fail(std::string message);
  bool reject(std::string message);
  bool writeBytes(const uint8_t* data, size_t size);

  std::unique_ptr<MuxerOutput> mOutput;
  MuxerOptions mOptions;
  Status mStatus = Status::kIdle;
  Error mError;
  int mTrackId = -1;
  int mSampleRate = 0;
  int mChannelCount = 0;
  int mBitsPerSample = 16;
  int mAudioFormat = 1; // 1 = PCM integer, 3 = IEEE float.
  int mBlockAlign = 0;
  int64_t mDataBytes = 0;
  int64_t mSampleCount = 0;
  bool mHeaderWritten = false;

  static constexpr int64_t kHeaderSize = 44;
};

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_WAV_MUXER_H
