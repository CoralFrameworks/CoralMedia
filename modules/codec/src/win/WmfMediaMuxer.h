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

#ifndef CORAL_CODEC_WMF_MEDIA_MUXER_H
#define CORAL_CODEC_WMF_MEDIA_MUXER_H

#include <codec/MediaMuxer.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * The implementation of the AVMediaMuxer based on the Windows Media Foundation sink writer.
 *
 * The sink writer writes the already encoded packets of the tracks into the container, the media
 * types of the tracks are declared by {@link addTrack}, so no additional encoder is inserted.
 */
class WmfMediaMuxer final : public AVMediaMuxer {
public:
  WmfMediaMuxer();

  ~WmfMediaMuxer() override;

  bool prepare(std::string url, MuxerFormat format) override;

  int addTrack(const std::shared_ptr<MediaFormat>& format) override;

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override;

  void start() override;

  void stop() override;

  void release() override;

  [[nodiscard]] const char* getName() const override {
    return "WmfMediaMuxer";
  }

  /**
   * Indicates whether the platform sink writer is able to write the container format.
   */
  static bool isSupportedFormat(MuxerFormat format);

private:
  /**
   * The state of a track which has been added to the sink writer.
   */
  struct TrackInfo {
    TrackInfo() = default;

    //!< Whether the track contains video data.
    bool video = false;
    //!< The duration of one packet, in 100ns units, 0 when it is unknown.
    int64_t packetDuration100ns = 0;
  };

  /**
   * A sample which waits for the samples of the other tracks, the sink writer only accepts the
   * samples in the order of their presentation time.
   */
  struct PendingSample {
    DWORD streamIndex = 0;
    int64_t timeUs = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
  };

  static bool convertToContainerType(MuxerFormat format, GUID& containerType);

  /**
   * Builds the compressed media type of the track.
   */
  static Microsoft::WRL::ComPtr<IMFMediaType> createTrackMediaType(
    const std::shared_ptr<MediaFormat>& format);

  /**
   * Writes the pending samples which are older than the reorder window.
   *
   * @param flushAll whether every pending sample is written.
   */
  void flushPendingSamples(bool flushAll);

  std::string mUrl;
  MuxerFormat mFormat = MuxerFormat::kMuxerFormatInvalid;

  Microsoft::WRL::ComPtr<IMFSinkWriter> mSinkWriter;
  std::vector<TrackInfo> mTracks;

  //!< The samples which are waiting to be written, sorted by the presentation time.
  std::vector<PendingSample> mPendingSamples;

  //!< The largest presentation time which has been received, in microseconds.
  int64_t mMaxPendingTimeUs = Definitions::kTimeUnset;

  bool mWmfStarted = false;
  bool mFinalized = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_MEDIA_MUXER_H
