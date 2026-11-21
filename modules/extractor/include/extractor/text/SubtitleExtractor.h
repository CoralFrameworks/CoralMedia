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

#ifndef CORAL_EXTRACTOR_TEXT_SUBTITLE_EXTRACTOR_H
#define CORAL_EXTRACTOR_TEXT_SUBTITLE_EXTRACTOR_H

#include <extractor/Extractor.h>

#include <cstddef>
#include <memory>
#include <string>

namespace coral {

/**
 * Extracts a standalone subtitle document, i.e. a `.vtt`, `.srt`, `.ssa` or `.ass` file.
 *
 * <p>This is the counterpart of ExoPlayer's {@code SubtitleExtractor}: the whole file is one sample
 * of one subtitle track, and the parser of the format turns that sample into the cues of the
 * document. The file is recognized by its content rather than by its extension, so a document that
 * was renamed is still handled, and a hint of the file type that the caller knows is only a
 * fallback.
 *
 * <p>The sample is delivered with a presentation time of zero, and the document states its own
 * cue times, which the player maps onto the item with
 * {@link MediaFormat::OFFSET_SAMPLE_RELATIVE}: the cues of the sample are placed at the times the
 * document states rather than at the time of the sample.
 */
class AVSubtitleExtractor final : public AVExtractor {
public:
  AVSubtitleExtractor() = default;

  ~AVSubtitleExtractor() override = default;

  AVSubtitleExtractor(const AVSubtitleExtractor& other) = delete;

  AVSubtitleExtractor& operator=(const AVSubtitleExtractor& other) = delete;

  /**
   * Returns {@code kSniffResultCertain} when the first bytes are the header of a document that this
   * extractor parses, and {@code kSniffResultNone} otherwise. The peek position of the input is
   * restored before returning.
   *
   * <p>The confidence is certain rather than possible because the signatures are the ones of a text
   * document: no container of the module starts with `WEBVTT`, with a SubRip timing line or with an
   * SSA header.
   */
  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  /**
   * Resets the extractor so that the sample is delivered again from the position the reader moved
   * the input to.
   */
  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] std::string getName() const override { return "Subtitle"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:

  /** Returns the MIME type of the document in @p data, or an empty string when it is not known. */
  [[nodiscard]] static std::string mimeTypeForDocument(const uint8_t* data, size_t size);

  AVExtractorOutput* output_ = nullptr;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  /** Whether the sample of the document was delivered. */
  bool delivered_ = false;
  std::string sniffFailureReason_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_SUBTITLE_EXTRACTOR_H
