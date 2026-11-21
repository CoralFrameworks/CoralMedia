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

#ifndef CORAL_EXTRACTOR_TEXT_SUBRIP_SUBRIP_PARSER_H
#define CORAL_EXTRACTOR_TEXT_SUBRIP_SUBRIP_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <cstddef>
#include <cstdint>

namespace coral {

/**
 * The parser of a SubRip document.
 *
 * <p>This is the counterpart of ExoPlayer's {@code SubripParser}: a document is a sequence of cues,
 * each with an optional index, a `start --> end` timing line and one or more lines of text. The
 * inline markup is a small HTML subset (`<b>`, `<i>`, `<u>`, `<s>`, `<font color>`), and the legacy
 * `{\anN}` tag of the format decides the alignment of the cue.
 *
 * <p>A container that stores one cue per sample (Matroska, for example) hands in a document that
 * holds the index and timing line the extractor synthesized, so the same parser serves a `.srt` file
 * and a subtitle track.
 */
class AVSubripParser final : public AVSubtitleParser {
public:
  /** The cue replacement behavior of the format, see {@link AVSubtitleParserFactory}. */
  static constexpr CueReplacementBehavior kCueReplacementBehavior = CueReplacementBehavior::kMerge;

  AVSubripParser() = default;

  ~AVSubripParser() override = default;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_SUBRIP_SUBRIP_PARSER_H
