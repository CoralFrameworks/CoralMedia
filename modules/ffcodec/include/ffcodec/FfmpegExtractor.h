/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_CODEC_FFMPEG_EXTRACTOR_H
#define CORAL_CODEC_FFMPEG_EXTRACTOR_H

#include <codec/MediaExtractor.h>
#include <codec/CodecTypes.h>
#include <media/Format.h>

#include <extractor/MediaExtractorInput.h>

#include "private/ffcodec/FfmpegUtils.h"

#include <vector>

namespace coral {

/**
 * The Extractor based on Ffmpeg
 */
class FfmpegExtractor final : public AVMediaExtractor {
public:
  /**
   * Creates a new extractor of the url, or null when the media is not a container ffmpeg reads.
   *
   * <p>The media is opened by this call, so the extractor reads it itself: this is what the legacy
   * reader, which drives an extractor without an input of its own, uses.
   */
  static std::shared_ptr<FfmpegExtractor> createWithUrl(const std::string& url);

  /**
   * Creates a new extractor of the media of an {@link AVExtractorInput}.
   *
   * <p>Nothing is opened here: the media is opened by {@link #sniff}, from the input of the reader,
   * through the callbacks this extractor installs on an {@code AVIOContext} of ffmpeg. The url is only
   * the one the extractor was created for, which is what its diagnostics report.
   */
  static std::shared_ptr<FfmpegExtractor> createForInput(std::string url);

  explicit FfmpegExtractor(AVFormatContext* formatContext) noexcept;

  ~FfmpegExtractor() override;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  PlatformResult read(PositionHolder& seekPosition) override;

  bool seekTo(int64_t timeUs, SeekOption option) override;

  void clear() override;

  [[nodiscard]] bool isSeekable() const override;

  [[nodiscard]] int64_t getDurationUs() const override;

  [[nodiscard]] int64_t getReadPosition() const override;

  [[nodiscard]] coral::SeekPoints getPlatformSeekPoints(int64_t timeUs) const override;

  [[nodiscard]] std::string getName() const override { return "FfmpegExtractor"; }

private:
  explicit FfmpegExtractor(std::string url) noexcept;

  void destroy();

  /**
   * Opens the media of the input of the reader with ffmpeg, through an {@code AVIOContext} whose reads
   * and seeks are the reads and the seeks of that input.
   */
  bool openFromInput(AVExtractorInput& input);

  size_t getTrackCount() const;

  void prepareFormat();

  std::shared_ptr<MediaFormat> getTrackFormat(int index);

  void parseVideoBitStream(AVPacket* pkt);

  void notifyEndOfStream();

  /**
   * Reads the bytes of the media of the reader, i.e. the read callback of the {@code AVIOContext}.
   */
  static int readPacket(void* opaque, uint8_t* buffer, int size);

  /**
   * Moves the media of the reader, i.e. the seek callback of the {@code AVIOContext}.
   */
  static int64_t seekInput(void* opaque, int64_t offset, int whence);

  /**
   * Returns whether is a real time format context
   */
  static bool isRealTime(AVFormatContext* pFormatCtx);

  struct AVBSFContextDeleter {
    void operator()(AVBSFContext* context) const { av_bsf_free(&context); }
  };

  std::string mInputUrl;
  //!< The input of the reader, when the media is read through it rather than opened by url. It is held
  //!< before the format context, so that the context which reads through it is released first.
  std::unique_ptr<AVMediaExtractorInput> mInput;
  //!< The io context of ffmpeg that reads the input of the reader, whose buffer it owns as well. A
  //!< context created with AVFMT_FLAG_CUSTOM_IO is not freed by avformat_close_input.
  AVIOContext* pIoContext = nullptr;
  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> pFormatContext;
  std::unique_ptr<AVPacket, AVPacketDeleter> pCachePacket;

  const AVBitStreamFilter* pVideoStreamFilter;
  std::unique_ptr<AVBSFContext, AVBSFContextDeleter> pVideoStreamContext;

  //!< The output of the reader that receives the tracks and the seek map. Owned by the reader, which
  //!< keeps it alive for as long as it drives this extractor, and never handed out again.
  AVExtractorOutput* pExtractorOutput = nullptr;
  bool mNeedPrepareFormat;
  bool mHasAudioTrack;
  bool mHasVideoTrack;
  int64_t mReadingPositionUs;
  int64_t mReadingPosition;

  class FfmpegTrack {
  public:
    explicit FfmpegTrack(const int trackIndex) noexcept
        : trackIndex(trackIndex),
          trackId(Definitions::kInvalidValue),
          mMediaType(MediaType::kMediaTypeInvalid) {}

    explicit FfmpegTrack(const int trackIndex,
        const int trackId,
        const MediaType type,
        std::shared_ptr<TrackOutput> trackOutput) noexcept
        : trackIndex(trackIndex),
          trackId(trackId),
          mMediaType(type),
          trackOutput(std::move(trackOutput)) {}

    void clear() { trackOutput.reset(); }

    [[nodiscard]] int getTrackIndex() const { return trackIndex; }

    /** Returns the identifier the track was declared with, which its samples carry. */
    [[nodiscard]] int getTrackId() const { return trackId; }

    [[nodiscard]] MediaType getMediaType() const { return mMediaType; }

    bool isValid() const { return trackIndex >= 0 && trackOutput != nullptr; }

    void changeFormat(const std::shared_ptr<MediaFormat>& format);

    void enqueuePacketData(AVPacket* avPacket, int64_t timeUs);

    void enqueueEndOfStream();

  private:
    int trackIndex;
    int trackId;
    MediaType mMediaType;
    std::shared_ptr<TrackOutput> trackOutput;
  };

  std::vector<FfmpegTrack> tracks;
};

class FfmpegExtractorFactory : public ExtractorFactory {
public:
  FfmpegExtractorFactory();

  ~FfmpegExtractorFactory() override;

  std::shared_ptr<AVMediaExtractor> createExtractor(const std::string& url, bool platform) override;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_EXTRACTOR_H
