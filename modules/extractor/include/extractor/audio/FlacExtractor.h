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

#ifndef CORAL_EXTRACTOR_AUDIO_FLAC_EXTRACTOR_H
#define CORAL_EXTRACTOR_AUDIO_FLAC_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/Utility.h>

#include <media/Metadata.h>

#include <memory>
#include <vector>

namespace coral {

/**
 * Extracts the frames of a native FLAC stream.
 *
 * <p>The extractor reads the {@code fLaC} marker and the metadata blocks that follow it, in
 * particular the mandatory STREAMINFO block, and then delivers the audio frames as they are. The
 * Vorbis comment block, the padding blocks and the seek table are skipped, because the seek map is
 * derived from the STREAMINFO block.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code FlacExtractor}.
 */
class AVFlacExtractor final : public AVExtractor {
public:
  AVFlacExtractor() = default;

  ~AVFlacExtractor() override = default;

  AVFlacExtractor(const AVFlacExtractor& other) = delete;

  AVFlacExtractor& operator=(const AVFlacExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "FLAC"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * The content of the STREAMINFO metadata block.
   */
  struct StreamInfo {
    /** The minimum block size, in samples. */
    int minBlockSize = 0;
    /** The maximum block size, in samples. */
    int maxBlockSize = 0;
    /** The minimum frame size, in bytes. */
    int minFrameSize = 0;
    /** The maximum frame size, in bytes. */
    int maxFrameSize = 0;
    /** The sample rate, in Hz. */
    int sampleRate = 0;
    /** The number of channels. */
    int channelCount = 0;
    /** The number of bits per sample. */
    int bitsPerSample = 0;
    /** The total number of samples per channel, or 0 when unknown. */
    int64_t totalSamples = 0;
  };

  /**
   * Reads the {@code fLaC} marker and the metadata blocks.
   *
   * @return {@code true} when the audio frames start at the current position.
   */
  bool readMetadata(AVExtractorInput& input);

  /**
   * Parses the 34 bytes of a STREAMINFO block.
   */
  void parseStreamInfo(const uint8_t data[], size_t size);

  /**
   * Reads the header of the frame that starts at the current peek position.
   *
   * @param input The input to peek from.
   * @param samplesInFrame The number of samples of the frame that is filled in.
   * @param frameSize The size of the frame in bytes that is filled in, or -1 when it is not
   *                  described by the header.
   *
   * @return {@code true} when a valid frame header was found.
   */
  bool peekFrameHeader(AVExtractorInput& input, int& samplesInFrame, int& frameSize);

  /**
   * Builds the format of the track from {@link streamInfo_}.
   */
  std::shared_ptr<MediaFormat> buildFormat() const;

  bool metadataRead_ = false;
  bool hasStreamInfo_ = false;
  bool formatPublished_ = false;
  bool trackEnded_ = false;
  bool seekMapPublished_ = false;
  StreamInfo streamInfo_;
  /** The metadata that the blocks of the stream declared, or null. */
  std::shared_ptr<Metadata> metadata_;
  int minimumFrameSize_ = 0;
  long firstFramePosition_ = -1;
  long position_ = 0;
  int64_t frameIndex_ = 0;
  int64_t samplesRead_ = 0;
  int64_t durationUs_ = Definitions::kTimeUnset;
  std::string sniffFailureReason_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  AVExtractorOutput* seekMapOutput_ = nullptr;
  AVDataBuffer scratch_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_AUDIO_FLAC_EXTRACTOR_H
