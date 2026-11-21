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

#ifndef CORAL_MUXER_MP4_MUXER_H
#define CORAL_MUXER_MP4_MUXER_H

#include <muxer/Muxer.h>

#include "Mp4Metadata.h"
#include "Mp4SampleEntry.h"
#include "MuxerOutput.h"
#include "SampleTable.h"

#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/**
 * The seekable, non-fragmented MP4 writer.
 *
 * Samples are buffered for a bounded interleaving window, written to a single `mdat` box, and the
 * sample tables and `moov` are written at `finish()`. With `fastStart`, the finished `mdat` is
 * moved behind the `moov` box before the file is closed.
 */
class Mp4Muxer final : public Muxer {
public:
  static std::unique_ptr<Mp4Muxer> create(const std::string& url, const MuxerOptions& options, Error* error);
  static std::unique_ptr<Mp4Muxer> create(std::unique_ptr<MuxerOutput> output,
    const MuxerOptions& options, Error* error);

  ~Mp4Muxer() override;

  int addTrack(const std::shared_ptr<MediaFormat>& format, const TrackOptions& options) override;

  bool writeSample(int trackId, const uint8_t* data, size_t size, const SampleInfo& info) override;

  bool addMetadata(const std::shared_ptr<Metadata>& metadata) override;

  bool finish() override;

  void cancel() override;

  [[nodiscard]] Status getStatus() const noexcept override { return mStatus; }

  [[nodiscard]] Error getError() const override { return mError; }

  [[nodiscard]] Container getContainer() const noexcept override { return Container::kMp4; }

  static std::vector<std::vector<uint8_t>> splitNalUnits(const std::vector<uint8_t>& data);

private:
  struct PendingSample {
    std::vector<uint8_t> data;
    int64_t presentationTimeUs = 0;
    int64_t durationUs = 0;
    uint32_t flags = 0;
  };

  struct Track {
    explicit Track(int32_t trackTimescale)
            : timescale(trackTimescale),
              table(trackTimescale) {}

    std::shared_ptr<MediaFormat> format;
    TrackOptions options;
    int id = -1;
    int32_t timescale = 90000;
    Codec codec = Codec::kUnknown;
    SampleTable table;
    std::vector<PendingSample> pending;
    int64_t pendingMinPts = 0;
    int64_t pendingMaxPts = 0;
    int64_t pendingBytes = 0;
    bool hadKeyFrame = false;
  };

  struct ExternalTrack {
    int id = -1;
    int cacheTrackId = -1;
    int type = 0;
  };

  Mp4Muxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept;

  bool writeFileHeader(std::string* error);
  bool writeBytes(const uint8_t* data, size_t size);
  bool flushTrack(Track& track);
  bool flushPendingTracks();
  bool maybeFlushTrack(Track& track);
  bool writeMoovBytes(std::vector<uint8_t>* bytes);
  void writeMoov(BoxWriter& writer);
  void writeTrack(BoxWriter& writer, const Track& track, int64_t globalFirstPts, int64_t movieTimescale);
  bool finalizePrimarySamples();
  bool finishSpooledFastStart();
  bool finishSpooledFastStartWithAuxiliaryTracks();
  bool finishWithAuxiliaryTracks();
  bool finishAuxiliaryMovie(uint64_t* axteSize);
  bool appendAuxiliaryMovie(uint64_t axteSize);
  bool copySpoolToOutput();
  bool addMetadataEntry(const std::string& key, std::vector<uint8_t> value,
    MdtaMetadataEntry::ValueType type);

  bool fail(std::string message);
  bool reject(std::string message);
  [[nodiscard]] Track* findTrack(int trackId);
  [[nodiscard]] const Track* findTrack(int trackId) const;
  [[nodiscard]] ExternalTrack* findExternalTrack(int trackId);

  std::unique_ptr<MuxerOutput> mOutput;
  std::unique_ptr<MuxerOutput> mSpool;
  std::string mSpoolPath;
  bool mSpoolMode = false;
  int64_t mFtypSize = 0;
  MuxerOptions mOptions;
  std::vector<Track> mTracks;
  std::vector<ExternalTrack> mExternalTracks;
  std::unique_ptr<Mp4Muxer> mAxteMuxer;
  std::string mAxtePath;
  Mp4Metadata mMetadata;
  Status mStatus = Status::kIdle;
  Error mError;
  int64_t mMdatStart = 0;
  int64_t mMdatEnd = 0;
  int mNextTrackId = 1;
  int64_t mGlobalFirstPts = 0;
};

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_MUXER_H
