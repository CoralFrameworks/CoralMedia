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

#ifndef CORAL_EXTRACTOR_AUDIO_AMR_EXTRACTOR_H
#define CORAL_EXTRACTOR_AUDIO_AMR_EXTRACTOR_H

#include <extractor/Extractor.h>

#include <memory>
#include <string>

namespace coral {

/**
 * Extracts the frames of an AMR or AMR-WB container, as defined by RFC 4867, section 5.
 *
 * <p>The stream starts with the {@code "#!AMR\n"} or {@code "#!AMR-WB\n"} signature, followed by a
 * sequence of frames. Every frame starts with a table of contents byte that declares its frame type,
 * which in turn declares the size of the frame and the bitrate at which it was encoded. The frame
 * duration is 20 milliseconds, therefore the timestamps do not depend on the frame type.
 *
 * <p>This extractor only supports the single channel container formats, which is what RFC 4867
 * defines. It stores the frames it reads in memory only for the duration of the read.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AmrExtractor}. The constant
 * bitrate and index seeking modes of the Java implementation are not exposed, because the file is
 * read forward and the seek map reports the duration once the end of the input was reached.
 */
class AVAmrExtractor final : public AVExtractor {
public:
  AVAmrExtractor() = default;

  ~AVAmrExtractor() override = default;

  AVAmrExtractor(const AVAmrExtractor& other) = delete;

  AVAmrExtractor& operator=(const AVAmrExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "AMR"; }

  [[nodiscard]] std::string getSniffFailureReason() const override { return sniffFailureReason_; }

private:
  /**
   * Reads the signature of the stream and consumes it.
   *
   * @return {@code true} when the signature was found.
   */
  bool readHeader(AVExtractorInput& input);

  /**
   * Builds the {@link MediaFormat} of the stream.
   */
  std::shared_ptr<MediaFormat> buildFormat() const;

  /**
   * Returns the size of the frame with the given type, or -1 when the type is not valid for the
   * stream.
   */
  [[nodiscard]] int getFrameSize(int frameType) const;

  /**
   * Reports the end of the track.
   */
  void finish(AVExtractorInput& input);

  bool headerRead_ = false;
  bool isWideBand_ = false;
  bool formatPublished_ = false;
  bool trackEnded_ = false;
  long position_ = 0;
  int64_t currentSampleTimeUs_ = 0;
  int64_t sampleCount_ = 0;
  /** The time that a seek which restarted the stream wants to skip to, or -1. */
  int64_t skipBeforeTimeUs_ = -1;
  std::string sniffFailureReason_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_AUDIO_AMR_EXTRACTOR_H
