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

#ifndef CORAL_EXTRACTOR_TEXT_CEA708_CEA708_PARSER_H
#define CORAL_EXTRACTOR_TEXT_CEA708_CEA708_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * Decodes CEA-708 (DTVCC) caption data into text cues.
 *
 * <p>The sample is the `cc_data` triplet stream of a CEA-708 track: three bytes per packet, the
 * first carrying `cc_valid` and `cc_type` and the other two the DTVCC payload. Service 1 is decoded;
 * the other services are skipped. Up to eight caption windows are tracked, including their pen
 * colours, italics, underline and justify settings.
 */
class AVCea708Parser final : public AVSubtitleParser {
public:
  /** Decodes service 1. */
  AVCea708Parser();
  /**
   * @param serviceNumber The DTVCC service to decode, 1 to 63.
   */
  explicit AVCea708Parser(int serviceNumber);
  ~AVCea708Parser() override;

  AVCea708Parser(const AVCea708Parser&) = delete;
  AVCea708Parser& operator=(const AVCea708Parser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CEA708_CEA708_PARSER_H
