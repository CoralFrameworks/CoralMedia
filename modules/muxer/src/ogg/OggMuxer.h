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

#ifndef CORAL_MUXER_OGG_MUXER_H
#define CORAL_MUXER_OGG_MUXER_H

#include "MuxerOutput.h"
#include "muxer/Muxer.h"

#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace ogg {

enum class OggCodec : uint8_t {
  kUnknown = 0,
  kOpus,
  kVorbis,
  kFlac,
};

/** An Ogg writer for a single Opus, Vorbis or FLAC stream. */
class OggMuxer final : public Muxer {
public:
  static std::unique_ptr<OggMuxer> create(const std::string& url, const MuxerOptions& options,
    Error* error);

  ~OggMuxer() override;

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
    return Container::kOgg;
  }

private:
  OggMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept;

  bool writeHeaders();
  bool writePacket(const std::vector<uint8_t>& packet, int64_t granulePosition, uint8_t flags);
  bool writePage(const std::vector<std::vector<uint8_t>>& packets, int64_t granulePosition,
    uint8_t flags);
  bool writePageChunk(const std::vector<uint8_t>& segments,
    const std::vector<uint8_t>& payload, int64_t granulePosition, uint8_t flags);
  bool fail(std::string message);
  bool reject(std::string message);
  bool writeBytes(const uint8_t* data, size_t size);

  std::unique_ptr<MuxerOutput> mOutput;
  MuxerOptions mOptions;
  Status mStatus = Status::kIdle;
  Error mError;
  std::shared_ptr<MediaFormat> mFormat;
  OggCodec mCodec = OggCodec::kUnknown;
  int mTrackId = -1;
  int mSampleRate = 48000;
  int64_t mPreSkip = 0;
  int64_t mGranulePosition = 0;
  uint32_t mSerial = 0;
  uint32_t mSequence = 0;
  bool mHeadersWritten = false;
  bool mHasSamples = false;
};

} // namespace ogg
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_OGG_MUXER_H
