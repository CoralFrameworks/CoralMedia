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

#ifndef CORAL_EXTRACTOR_TEXT_PGS_PGS_PARSER_H
#define CORAL_EXTRACTOR_TEXT_PGS_PGS_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * Parses a PGS (Presentation Graphic Stream) display set into bitmap cues.
 *
 * <p>The parser accepts the segment stream that a Matroska PGS track stores: palette (PDS), object
 * (ODS), presentation composition (PCS) and window (WDS) segments, terminated by END. The object
 * run-length data is expanded with the palette of the current display set, and the resulting object
 * is placed at the composition position in an RGBA bitmap.
 *
 * <p>Window definitions clip composition objects, a PCS cropping rectangle selects the visible
 * part of an object, and a PDS that arrives after an ODS is applied when the display set is
 * composed. An ODS may also be split across first/last segments, and a later ODS version replaces
 * the object with the same id. Object decoding is therefore deferred until composition time.
 */
class AVPgsParser final : public AVSubtitleParser {
public:
  AVPgsParser();
  ~AVPgsParser() override;

  AVPgsParser(const AVPgsParser&) = delete;
  AVPgsParser& operator=(const AVPgsParser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_PGS_PGS_PARSER_H
