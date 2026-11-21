/*
 * Copyright (C) 2023 The Coral Project
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

#ifndef CORAL_CODEC_ANDROID_NATIVE_EXTRACTOR_H
#define CORAL_CODEC_ANDROID_NATIVE_EXTRACTOR_H

#include <codec/MediaExtractor.h>
#include <codec/CodecTypes.h>

#include <extractor/MediaExtractorInput.h>

#if defined(__ANDROID__)
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#endif

#include <memory>

namespace coral {

/**
 * The Extractor implementation based on AMediaExtractor
 */
class NativeExtractor final : public AVMediaExtractor {
public:
  /**
   * Creates a new extractor of the url, or null when the url is not a supported media file.
   *
   * <p>The media is opened by this call, so the extractor reads it itself: this is what the legacy
   * reader, which drives an extractor without an input of its own, uses.
   *
   * <p>The extractor is shared rather than owned by the caller alone: it hands itself out as the seek
   * map of the media it reads, so it has to be owned by a shared pointer.
   */
  static std::shared_ptr<NativeExtractor> CreateWithUrl(const std::string& url);

  /**
   * Creates a new extractor of the media of an {@link AVExtractorInput}.
   *
   * <p>Nothing is opened here: the media is opened by {@link #sniff}, from the input of the reader,
   * through the custom data source of the platform extractor. The url is only the one the extractor was
   * created for, which is what its diagnostics report.
   *
   * <p>Returns null when the platform is older than the custom data source of the NDK extractor, which
   * was introduced by the API level 28: the caller then falls back to an extractor of the url.
   */
  static std::shared_ptr<NativeExtractor> CreateForInput(std::string url);

  NativeExtractor();

  ~NativeExtractor() override;

  bool setDataSource(const std::string& url);

  int sniff(AVExtractorInput& input) override;

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

  int getTrackCount();

  std::shared_ptr<MediaFormat> getTrackFormat(int trackIndex);

  [[nodiscard]] std::string getName() const override { return "NdkExtractor"; }

private:
  /**
   * Opens the media of the input of the reader, through a data source of the NDK whose reads are the
   * reads of that input.
   */
  bool openFromInput(AVExtractorInput& input);

  /**
   * Reads the media of the reader at the given position, i.e. the read callback of the data source.
   */
  static ssize_t readAt(void* userdata, off64_t position, void* buffer, size_t size);

  /**
   * Returns the length of the media of the reader, i.e. the size callback of the data source, or -1
   * when it does not report one.
   */
  static ssize_t getSize(void* userdata);

  /**
   * Releases what the data source of the media of the reader holds, i.e. its close callback. The input
   * of the reader is owned by the reader, so nothing is released here.
   */
  static void closeSource(void* userdata);

  void prepareFormat();

  static constexpr int DEFAULT_MAX_INPUT_SIZE = 2048 * 2048;

  //!< AudioFormat::ENCODING_PCM_16BIT
  static constexpr int ENCODING_PCM_16BIT = 2;

  //!< AudioFormat::ENCODING_PCM_FLOAT
  static constexpr int ENCODING_PCM_FLOAT = 4;

  //!< AudioFormat::ENCODING_PCM_24BIT_PACKED
  static constexpr int ENCODING_PCM_24BIT_PACKED = 21;

  //!< AudioFormat::ENCODING_PCM_32BIT
  static constexpr int ENCODING_PCM_32BIT = 22;

  struct AMediaExtractorDeleter {
    void operator()(AMediaExtractor* extractor) const {
      if (extractor) {
        AMediaExtractor_delete(extractor);
      }
    }
  };

  std::unique_ptr<AMediaExtractor, AMediaExtractorDeleter> mExtractor;
  //!< The input of the reader, when the media is read through it rather than opened by url, and the data
  //!< source of the platform extractor that reads it. The data source is created by this extractor, which
  //!< is the one that deletes it, see AMediaDataSource_delete.
  std::unique_ptr<AVMediaExtractorInput> mInput;
  AMediaDataSource* mDataSource = nullptr;
  //!< The output of the reader that receives the tracks and the seek map. Owned by the reader, which
  //!< keeps it alive for as long as it drives this extractor, and never handed out again.
  AVExtractorOutput* mOutput = nullptr;
  bool needPrepareFormat;
  int64_t durationUs = Definitions::kTimeUnset;
  int64_t mCurrentPosition;

  std::unique_ptr<uint8_t[]> mData;
  // ByteBuffer allocation size
  int maxInputSize = DEFAULT_MAX_INPUT_SIZE;

  class MediaTrack {
  public:
    explicit MediaTrack(const int trackIndex) noexcept
      : trackIndex(trackIndex),
        trackId(Definitions::kInvalidValue) {
    }

    explicit MediaTrack(const int trackIndex, const int trackId, std::shared_ptr<TrackOutput> trackOutput) noexcept
      : trackIndex(trackIndex),
        trackId(trackId),
        trackOutput(std::move(trackOutput)) {
    }

    void clear() { trackOutput.reset(); }

    [[nodiscard]] int getTrackIndex() const { return trackIndex; }

    [[nodiscard]] int getTrackId() const { return trackId; }

    bool isValid() const {
      return trackIndex >= 0 && trackId >= 0 && trackOutput != nullptr;
    }

    void changeFormat(const std::shared_ptr<MediaFormat>& format) const;

    void sampleData(const uint8_t* data, int size, int64_t timeUs, AVBufferFlags flags) const;

  private:
    int trackIndex;
    int trackId;
    std::shared_ptr<TrackOutput> trackOutput;
  };

  std::vector<MediaTrack> tracks;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_NATIVE_EXTRACTOR_H
