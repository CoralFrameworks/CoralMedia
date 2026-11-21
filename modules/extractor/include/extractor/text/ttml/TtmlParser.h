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

#ifndef CORAL_EXTRACTOR_TEXT_TTML_TTML_PARSER_H
#define CORAL_EXTRACTOR_TEXT_TTML_TTML_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * Parses the TTML/IMSC documents of a subtitle track.
 *
 * <p>The parser covers the text and timing subset that a side-loaded TTML document and an MP4
 * {@code stpp} track use in practice: nested {@code <p>} paragraphs, {@code <br/>} line breaks,
 * {@code begin}/{@code end}/{@code dur} timing and the inline {@code tts:color},
 * {@code tts:fontStyle}, {@code tts:fontWeight} and {@code tts:textDecoration} attributes. Region
 * layout, animation and ruby annotations are left for a later pass; a document that only uses them
 * still produces its text.
 */
class AVTtmlParser final : public AVSubtitleParser {
public:
  AVTtmlParser();
  ~AVTtmlParser() override;

  AVTtmlParser(const AVTtmlParser&) = delete;
  AVTtmlParser& operator=(const AVTtmlParser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_TTML_TTML_PARSER_H
