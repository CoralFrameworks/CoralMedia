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

#ifndef CORAL_CODEC_ANDROID_MEDIA_EXTRACTOR_H
#define CORAL_CODEC_ANDROID_MEDIA_EXTRACTOR_H

#include <codec/MediaExtractor.h>
#include <codec/CodecTypes.h>

#include <core/jni/android/MediaExtractor.h>

#include <memory>

namespace coral {

/**
 * The Extractor implementation based on android.media.MediaExtractor
 */
class MediaExtractor final : public AVMediaExtractor {
public:
  /**
   * Creates a new extractor of the url.
   *
   * <p>The extractor is shared rather than owned by the caller alone: it hands itself out as the seek
   * map of the media it reads, so it has to be owned by a shared pointer.
   */
  static std::shared_ptr<MediaExtractor> CreateWithUrl(const std::string& url);

  MediaExtractor();

  ~MediaExtractor() override;

  void setDataSource(const std::string& url);

  void init(AVExtractorOutput& output) override;

  PlatformResult read(PositionHolder& seekPosition) override;

  bool seekTo(int64_t timeUs, SeekOption option) override;

  void clear() override;

  [[nodiscard]] bool isSeekable() const override {
    return durationUs != Definitions::kTimeUnset;
  }

  [[nodiscard]] int64_t getDurationUs() const override;

  [[nodiscard]] int64_t getReadPosition() const override;

  [[nodiscard]] coral::SeekPoints getPlatformSeekPoints(int64_t timeUs) const override;

  std::shared_ptr<MediaFormat> getTrackFormat(int trackIndex);

  [[nodiscard]] std::string getName() const override { return "MediaExtractor"; }

private:
  void prepareFormat();

  static constexpr int DEFAULT_MAX_INPUT_SIZE = 2048 * 2048;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  //!< AudioFormat::ENCODING_PCM_16BIT
  static constexpr int ENCODING_PCM_16BIT = 2;

  //!< AudioFormat::ENCODING_PCM_FLOAT
  static constexpr int ENCODING_PCM_FLOAT = 4;

  //!< AudioFormat::ENCODING_PCM_24BIT_PACKED
  static constexpr int ENCODING_PCM_24BIT_PACKED = 21;

  //!< AudioFormat::ENCODING_PCM_32BIT
  static constexpr int ENCODING_PCM_32BIT = 22;

  // ByteBuffer allocation size
  int maxInputSize = DEFAULT_MAX_INPUT_SIZE;

  std::unique_ptr<JniMediaExtractor> extractor;

  //!< The output of the reader that receives the tracks and the seek map. Owned by the reader, which
  //!< keeps it alive for as long as it drives this extractor, and never handed out again.
  AVExtractorOutput* mOutput = nullptr;
  bool needPrepareFormat;
  int64_t durationUs = Definitions::kTimeUnset;
  int64_t mCurrentPosition;

  class MediaTrack {
  public:
    explicit MediaTrack(const int trackIndex)
      : trackIndex(trackIndex), trackId(Definitions::kInvalidValue) {
    }

    explicit MediaTrack(const int trackIndex, const int trackId, const std::shared_ptr<TrackOutput>& trackOutput)
      : trackIndex(trackIndex),
        trackId(trackId),
        trackOutput(std::move(trackOutput)) {
    }

    void clear() { trackOutput.reset(); }

    int getTrackIndex() const { return trackIndex; }

    int getTrackId() const { return trackId; }

    bool isValid() const {
      return trackIndex >= 0 && trackId >= 0 && trackOutput != nullptr;
    }

    void changeFormat(const std::shared_ptr<MediaFormat>& format);

    void sampleData(const uint8_t* data, int size, int64_t timeUs, AVBufferFlags flags);

  private:
    int trackIndex;
    int trackId;
    std::shared_ptr<TrackOutput> trackOutput;
  };

  std::vector<MediaTrack> tracks;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIA_EXTRACTOR_H
