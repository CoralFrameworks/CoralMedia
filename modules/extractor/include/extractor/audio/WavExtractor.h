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

#ifndef CORAL_EXTRACTOR_WAV_EXTRACTOR_H
#define CORAL_EXTRACTOR_WAV_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>

#include <memory>

namespace coral {

/**
 * Extracts the PCM samples of a Waveform Audio File (WAV), as defined by the RIFF container.
 *
 * <p>The extractor supports the WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT, WAVE_FORMAT_ALAW and
 * WAVE_FORMAT_MULAW formats, which are the ones a media player can render without a decoder. The
 * {@code LIST}/{@code INFO} chunks, the {@code fact} chunk and every unknown chunk are skipped.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code WavExtractor}.
 */
class AVWavExtractor final : public AVExtractor {
public:
  AVWavExtractor() = default;

  ~AVWavExtractor() override = default;

  AVWavExtractor(const AVWavExtractor& other) = delete;

  AVWavExtractor& operator=(const AVWavExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "WAV"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * The state of the reader.
   */
  enum class State {
    /** The RIFF header has not been read yet. */
    kReadingHeader,
    /** The samples of the data chunk are being read. */
    kReadingData,
  };

  /**
   * The description of the audio data, as read from the {@code fmt} chunk.
   */
  struct AudioFormat {
    /** The WAVE_FORMAT_* identifier. */
    int codec;
    /** The number of audio channels. */
    int channelCount;
    /** The sample rate in Hz. */
    int sampleRate;
    /** The average number of bytes per second. */
    int byteRate;
    /** The number of bytes of one audio frame, i.e. of one sample of every channel. */
    int blockAlign;
    /** The number of bits per sample. */
    int bitsPerSample;
  };

  /**
   * Reads the RIFF header, the {@code fmt} chunk and the position of the {@code data} chunk.
   *
   * @return {@code true} when the header was consumed.
   */
  bool readHeaders(AVExtractorInput& input);

  /**
   * Reads the samples of the data chunk.
   */
  Result readData(AVExtractorInput& input);

  /**
   * Reads and returns the size of a RIFF chunk, honouring the RF64 extensions.
   *
   * @param chunkSize The 32 bit size stored in the chunk header.
   *
   * @return The real size of the chunk.
   */
  long getChunkSize(uint32_t chunkSize);

  /**
   * Builds the {@link MediaFormat} of the track from {@link format_}.
   */
  std::shared_ptr<MediaFormat> buildFormat() const;

  State state_ = State::kReadingHeader;
  AudioFormat format_ = {};
  bool hasFormat_ = false;
  bool formatPublished_ = false;
  long dataStartPosition_ = -1;
  long dataEndPosition_ = -1;
  long rf64DataSize_ = -1;
  /** The position of the input that the extractor reached, as reported by {@link getPosition}. */
  long position_ = 0;
  long totalBytesRead_ = 0;
  int64_t durationUs_ = Definitions::kTimeUnset;
  bool trackEnded_ = false;
  std::string sniffFailureReason_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  /** The output, kept so that an accurate seek map can be declared once the header was parsed. */
  AVExtractorOutput* output_ = nullptr;
  AVDataBuffer scratch_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_WAV_EXTRACTOR_H
