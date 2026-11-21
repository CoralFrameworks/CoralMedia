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

#ifndef CORAL_EXTRACTOR_TEXT_DVBSUB_DVBSUB_PARSER_H
#define CORAL_EXTRACTOR_TEXT_DVBSUB_DVBSUB_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * Parses a DVB subtitle segment stream into bitmap cues.
 *
 * <p>Supports the display definition, page composition, region composition, CLUT definition and
 * object data segments of ETSI EN 300 743. Objects are drawn into one RGBA bitmap per region; the
 * region position and size are published as fractions of the display definition.
 */
class AVDvbSubParser final : public AVSubtitleParser {
public:
  AVDvbSubParser();
  ~AVDvbSubParser() override;

  AVDvbSubParser(const AVDvbSubParser&) = delete;
  AVDvbSubParser& operator=(const AVDvbSubParser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_DVBSUB_DVBSUB_PARSER_H
