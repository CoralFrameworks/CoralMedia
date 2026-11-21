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

#ifndef CORAL_EXTRACTOR_TEXT_VOBSUB_VOBSUB_PARSER_H
#define CORAL_EXTRACTOR_TEXT_VOBSUB_VOBSUB_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>
#include <vector>

namespace coral {

/**
 * Parses a VobSub SPU packet into a bitmap cue.
 *
 * <p>The palette, canvas size and origin come from the `.idx` text that Matroska stores as the
 * codec private data of an {@code S_VOBSUB} track. Each sample is one SPU packet: its control
 * sequence is read for the palette/alpha indexes and the display rectangle, and the two RLE fields
 * are expanded into an RGBA bitmap.
 */
class AVVobSubParser final : public AVSubtitleParser {
public:
  AVVobSubParser();
  explicit AVVobSubParser(const std::vector<std::vector<uint8_t>>& codecPrivate);
  ~AVVobSubParser() override;

  AVVobSubParser(const AVVobSubParser&) = delete;
  AVVobSubParser& operator=(const AVVobSubParser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_VOBSUB_VOBSUB_PARSER_H
