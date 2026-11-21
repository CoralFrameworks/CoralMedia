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

#ifndef CORAL_EXTRACTOR_TEXT_TX3G_TX3G_PARSER_H
#define CORAL_EXTRACTOR_TEXT_TX3G_TX3G_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace coral {

/**
 * The parser of a 3GPP timed text (`tx3g`) sample of an MP4 container.
 *
 * <p>This is the counterpart of ExoPlayer's {@code Tx3gParser}. A sample is a two byte length, a
 * text, and then the optional `styl` and `tbox` boxes that style it. The default style of the track
 * belongs to its sample description rather than to the sample, so the parser reads it from the
 * initialization data that the default factory passes from the codec specific data of the format.
 *
 * <p>The sample of the format has no timing of its own: the cue is shown over the interval of the
 * container sample, and a sample whose text is empty clears the screen, which is why the replacement
 * behavior of the format is {@link CueReplacementBehavior::kReplace}.
 */
class AVTx3gParser final : public AVSubtitleParser {
public:
  /** The cue replacement behavior of the format, see {@link AVSubtitleParserFactory}. */
  static constexpr CueReplacementBehavior kCueReplacementBehavior = CueReplacementBehavior::kReplace;

  /**
   * @param initializationData The content of the sample description of the track, starting at the
   *                           `displayFlags` field, i.e. without the box header and the eight bytes
   *                           of the sample entry that precede it. An empty list falls back to the
   *                           default style of the specification.
   */
  explicit AVTx3gParser(const std::vector<std::vector<uint8_t>>& initializationData = {});

  ~AVTx3gParser() override;

  AVTx3gParser(const AVTx3gParser& other) = delete;

  AVTx3gParser& operator=(const AVTx3gParser& other) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  struct Impl;

  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_TX3G_TX3G_PARSER_H
