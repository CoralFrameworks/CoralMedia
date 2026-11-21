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

#ifndef CORAL_CODEC_WMF_MEDIA_EXTRACTOR_H
#define CORAL_CODEC_WMF_MEDIA_EXTRACTOR_H

#include <codec/Error.h>
#include <codec/MediaExtractor.h>

#include <extractor/MediaExtractorInput.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * The implementation of the AVMediaExtractor based on the Windows Media Foundation source reader.
 *
 * The extractor delivers the compressed (native) sample data of the audio and video streams, so the
 * demuxed packets can be decoded by an {@link AVMediaDecoder} implementation.
 */
class WmfMediaExtractor final : public AVMediaExtractor {
public:
  /**
   * Creates a new extractor of the url, returns nullptr when the url is not a supported media file.
   *
   * <p>The media is opened by this call, so the extractor reads it itself: this is what the legacy
   * reader, which drives an extractor without an input of its own, uses.
   *
   * <p>The extractor is shared rather than owned by the caller alone: it hands itself out as the seek
   * map of the media it reads, so it has to be owned by a shared pointer.
   */
  static std::shared_ptr<WmfMediaExtractor> createWithUrl(const std::string& url);

  /**
   * Creates a new extractor of the media of an {@link AVExtractorInput}.
   *
   * <p>Nothing is opened here: the media is opened by {@link #sniff}, from the input of the reader, and
   * the url is only the one the extractor was created for, which is what its diagnostics report.
   *
   * <p>The extractor is shared rather than owned by the caller alone: it hands itself out as the seek
   * map of the media it reads, so it has to be owned by a shared pointer.
   */
  static std::shared_ptr<WmfMediaExtractor> createForInput(std::string url);

  ~WmfMediaExtractor() override;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  PlatformResult read(PositionHolder& seekPosition) override;

  bool seekTo(int64_t timeUs, SeekOption option) override;

  void clear() override;

  [[nodiscard]] int64_t getDurationUs() const override {
    return mDurationUs;
  }

  [[nodiscard]] int64_t getReadPosition() const override {
    return mCurrentPosition;
  }

  // The SeekMap implementation.
  [[nodiscard]] bool isSeekable() const override {
    return mSeekable;
  }

  [[nodiscard]] coral::SeekPoints getPlatformSeekPoints(int64_t timeUs) const override;

  [[nodiscard]] std::string getName() const override { return "WmfMediaExtractor"; }

private:
  struct Track {
    //!< The stream index of the source reader.
    DWORD streamIndex = 0;
    //!< The track id reported to the caller.
    int trackId = Definitions::kInvalidValue;
    //!< The audio/video media type of the track.
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    //!< The track output.
    std::shared_ptr<TrackOutput> output;
    //!< Whether samples should be read from the stream.
    bool selected = false;
    //!< Whether the stream reached its end.
    bool endOfStream = false;
    //!< The sample read from the stream but not delivered yet.
    Microsoft::WRL::ComPtr<IMFSample> pendingSample;
    //!< The presentation timestamp (us) of the pending sample.
    int64_t pendingTimeUs = 0;
    //!< Whether the pending sample is a sync sample.
    bool pendingKeyFrame = false;
  };

  explicit WmfMediaExtractor(std::string url);

  /**
   * Opens the media of the url this extractor was created for.
   */
  bool setDataSource(Error** outError);

  /**
   * Opens the media of the input of the reader, i.e. the media this extractor demuxes when it was
   * created by {@link #createForInput}.
   */
  bool openFromInput(AVExtractorInput& input, Error** outError);

  /**
   * Creates the source reader of the media, either from the url or from the byte stream of the input
   * of the reader, and reads the duration and the seekability of the media.
   *
   * @param url The url of the media, or null when the media is read from a byte stream.
   * @param byteStream The stream of the media of the reader, or null when the media is opened by url.
   */
  bool createReader(const wchar_t* url, IMFByteStream* byteStream, Error** outError);

  bool prepareTracks();

  /**
   * Reads a sample of the tracks which are not at the end of the stream yet.
   *
   * @return false when every stream reached the end of the stream.
   */
  bool readPendingSample();

  /**
   * Delivers the pending sample of the track to the track output.
   */
  void emitSample(Track& track);

  /**
   * Indicates whether the pending sample should be dropped because of a pending
   * SEEK_TO_NEXT_SYNC request.
   */
  bool shouldDropSample(const Track& track) const;

  static constexpr int MAX_READ_RETRY = 4096;

  std::string mUrl;
  //!< The input of the reader, when the media is read through it rather than opened by url. It is held
  //!< before the source reader, so that the reader which reads through it is released first.
  std::unique_ptr<AVMediaExtractorInput> mInput;
  Microsoft::WRL::ComPtr<IMFSourceReader> mReader;
  //!< The output of the reader that receives the tracks and the seek map. Owned by the reader, which
  //!< keeps it alive for as long as it drives this extractor, and never handed out again.
  AVExtractorOutput* mOutput = nullptr;

  std::vector<Track> mTracks;

  int64_t mDurationUs = Definitions::kTimeUnset;
  int64_t mCurrentPosition = Definitions::kPositionUnset;
  int64_t mNextSyncTimeUs = Definitions::kTimeUnset;
  bool mSeekable = false;
  bool mPrepared = false;
  bool mWmfStarted = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_MEDIA_EXTRACTOR_H
