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

#ifndef CORAL_EXTRACTOR_TEXT_WEBVTT_WEBVTT_PARSER_H
#define CORAL_EXTRACTOR_TEXT_WEBVTT_WEBVTT_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace coral {

/**
 * The parser of a WebVTT file.
 *
 * <p>This is the counterpart of ExoPlayer's {@code WebvttParser}, and it parses a whole WebVTT
 * document: a `WEBVTT` header, optional `NOTE`/`STYLE`/`REGION` blocks and the cues with their
 * settings and markup. A container that stores one cue per sample (Matroska, for example) hands in
 * a synthesized document that holds the header and one cue, so the same parser serves both a
 * standalone `.vtt` file and a subtitle track.
 *
 * <p>The `STYLE` blocks of a document are skipped rather than evaluated: they are a CSS subset, and
 * the classes that this parser understands are the default colours of the specification
 * (`<c.yellow>`), not the ones a stylesheet declares.
 */
class AVWebvttParser final : public AVSubtitleParser {
public:
  /** The cue replacement behavior of the format, see {@link AVSubtitleParserFactory}. */
  static constexpr CueReplacementBehavior kCueReplacementBehavior = CueReplacementBehavior::kMerge;

  AVWebvttParser() = default;

  ~AVWebvttParser() override = default;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_WEBVTT_WEBVTT_PARSER_H
