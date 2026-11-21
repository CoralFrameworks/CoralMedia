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

#ifndef CORAL_CODEC_ANDROID_NDK_AUDIO_FILE_READER_H
#define CORAL_CODEC_ANDROID_NDK_AUDIO_FILE_READER_H

#include <codec/AudioFileReader.h>

#include <media/BlockBuffer.h>

#if defined(ANDROID) || defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#endif

#include <atomic>
#include <memory>
#include <string>

namespace coral {

/**
 * The implementation of the AudioFileReader based on AMediaExtractor and AMediaCodec.
 */
class NdkAudioFileReader final : public AudioFileReader {
public:
  static std::unique_ptr<NdkAudioFileReader> sniff(const std::string& inputUrl);

  NdkAudioFileReader(std::string inputUrl, AMediaExtractor* extractor, AMediaCodec* codec, int streamIndex) noexcept;

  ~NdkAudioFileReader() override = default;

  int64_t getSampleLength() const override;

  int64_t getSampleRate() const override;

  int32_t getChannelCount() const override;

  int64_t getFramePosition() const override;

  AudioSampleFormat getSampleFormat() const override;

  bool open(Error** outError) override;

  void seekTo(int64_t framePosition) override;

  void close() override;

  int64_t readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) override;

private:
  void prepareAudioFormats();

  /**
   * Feeds one sample of the extractor into the decoder.
   *
   * @return false when the decoder has no free input buffer right now.
   */
  bool feedInputBuffer();

  /**
   * Re-reads the audio format the decoder produces.
   *
   * @discussion The decoder can change its output format while it decodes, for example when the
   * first frame carries the real channel count or sample rate of the stream.
   *
   * @return true when the number of bytes per frame changed.
   */
  bool refreshOutputFormat();

  static AMediaExtractor* createExtractor(const std::string& inputUrl);

  static AMediaCodec* createCodec(AMediaExtractor* extractor, int* streamIndex);

  static const char* getName() { return "NdkAudioFileReader"; }

  static constexpr int TIME_OUT_US = 10000;

  /** How many empty decoder polls are tolerated in one read before giving up. */
  static constexpr int kMaxIdleRounds = 50;

  struct AMediaExtractorDeleter {
    void operator()(AMediaExtractor* extractor) const {
      if (extractor) {
        AMediaExtractor_delete(extractor);
      }
    }
  };

  struct AMediaCodecDeleter {
    void operator()(AMediaCodec* codec) const {
      if (codec != nullptr) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
      }
    }
  };

  std::string mInputUrl;
  int mStreamIndex;
  int64_t mSampleLength;
  int64_t mSampleRate;
  int32_t mChannelCount;
  int64_t mFramePosition;
  AudioSampleFormat mSampleFormat;
  uint32_t mBytesPerFrame;

  std::unique_ptr<AMediaExtractor, AMediaExtractorDeleter> mExtractor;
  std::unique_ptr<AMediaCodec, AMediaCodecDeleter> mMediaCodec;
  std::atomic_bool mPrepared;
  std::atomic_bool mInputEof;
  std::atomic_bool mOutputEof;

  AVBlockBuffer mCacheBuffer;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_NDK_AUDIO_FILE_READER_H
