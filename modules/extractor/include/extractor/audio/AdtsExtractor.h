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

#ifndef CORAL_EXTRACTOR_AUDIO_ADTS_EXTRACTOR_H
#define CORAL_EXTRACTOR_AUDIO_ADTS_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/Utility.h>

#include <media/Metadata.h>
#include <extractor/audio/AdtsReader.h>

#include <memory>

namespace coral {

/**
 * Extracts the frames of an ADTS stream, i.e. of an AAC elementary stream that is framed by a
 * seven byte ADTS header.
 *
 * <p>The extractor skips the ID3 tags, resynchronizes on the next frame when the stream is damaged,
 * and synthesizes the AudioSpecificConfig that the AAC decoder needs from the header of the first
 * frame.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AdtsExtractor}.
 */
class AVAdtsExtractor final : public AVExtractor {
public:
  AVAdtsExtractor() = default;

  ~AVAdtsExtractor() override = default;

  AVAdtsExtractor(const AVAdtsExtractor& other) = delete;

  AVAdtsExtractor& operator=(const AVAdtsExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "ADTS"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * Searches for the next ADTS frame header and parses it.
   *
   * @param input The input to read from.
   * @param header The header that is filled in on success.
   * @param framePosition The position of the first byte of the frame, filled in on success.
   *
   * @return {@code true} when a valid header was found.
   */
  bool findNextFrame(AVExtractorInput& input, AVAdtsHeader& header, long& framePosition);

  /** Reads an ID3 tag at the current position, if any, and emits it as a timed sample. */
  bool readMidStreamId3(AVExtractorInput& input, int64_t timeUs);

  /**
   * Builds the {@link MediaFormat} of the track, including the AudioSpecificConfig.
   */
  std::shared_ptr<MediaFormat> buildFormat(const AVAdtsHeader& header) const;

  /**
   * Reads and discards the ID3 tags at the start of the stream.
   */
  bool skipId3Headers(AVExtractorInput& input);

  bool initialized_ = false;
  bool id3Skipped_ = false;
  /** The metadata that the ID3 tags of the stream declared, or null. */
  std::shared_ptr<Metadata> metadata_;
  /** The raw ID3v2 tag bytes, kept for the timed metadata track. */
  std::vector<uint8_t> id3Tags_;
  bool formatPublished_ = false;
  bool trackEnded_ = false;
  bool tracksEnded_ = false;
  long position_ = 0;
  int64_t sampleCount_ = 0;
  int sampleRate_ = -1;
  int channelCount_ = 0;
  int64_t durationUs_ = Definitions::kTimeUnset;
  std::string sniffFailureReason_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  std::shared_ptr<AVTrackOutput> metadataOutput_;
  AVExtractorOutput* output_ = nullptr;
  AVDataBuffer scratch_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_AUDIO_ADTS_EXTRACTOR_H
