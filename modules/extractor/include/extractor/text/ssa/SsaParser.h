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

#ifndef CORAL_EXTRACTOR_TEXT_SSA_SSA_PARSER_H
#define CORAL_EXTRACTOR_TEXT_SSA_SSA_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace coral {

/**
 * The parser of an SSA/ASS document.
 *
 * <p>This is the counterpart of ExoPlayer's {@code SsaParser}. A document states its script
 * resolution and its styles in a header, then one `Dialogue:` line per cue. The inline overrides
 * (`{\i1}`, `{\c&HBBGGRR&}`, `{\an8}`, `{\pos(x,y)}`) change the style of the text that follows them
 * or the placement of the whole cue.
 *
 * <p>A subtitle track of a container states the header once and then one dialogue line per sample.
 * That case is expressed by passing the header as initialization data, which the constructor of the
 * parser takes and the default factory fills from the codec specific data of the format.
 */
class AVSsaParser final : public AVSubtitleParser {
public:
  /** The cue replacement behavior of the format, see {@link AVSubtitleParserFactory}. */
  static constexpr CueReplacementBehavior kCueReplacementBehavior = CueReplacementBehavior::kMerge;

  /**
   * Creates a parser that reads the header of every document it is given.
   */
  AVSsaParser();

  /**
   * Creates a parser for a subtitle track that states its header once.
   *
   * @param initializationData The codec specific data of the format: the `Format:` line of the
   *                           `[Events]` section first, then the header (everything before
   *                           `[Events]`). An empty list means "no header", i.e. the same as the
   *                           default constructor.
   */
  explicit AVSsaParser(const std::vector<std::vector<uint8_t>>& initializationData);

  ~AVSsaParser() override;

  AVSsaParser(const AVSsaParser& other) = delete;

  AVSsaParser& operator=(const AVSsaParser& other) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  struct Impl;

  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_SSA_SSA_PARSER_H
