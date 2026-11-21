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

#ifndef CORAL_EXTRACTOR_TEXT_CEA608_CEA608_PARSER_H
#define CORAL_EXTRACTOR_TEXT_CEA608_CEA608_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * Decodes CEA-608 caption byte pairs into text cues.
 *
 * <p>The sample is a sequence of 2-byte CC packets without the cc_data header. Pop-on, paint-on and
 * roll-up modes are supported at the text level; colours, italics and special characters are not
 * decoded yet.
 *
 * <p>CEA-608 carries two text channels in each field. The parser decodes one selected channel; the
 * default is channel 1, matching the reference decoder. Control codes for the other channel still
 * update the current channel but their text and commands are ignored.
 */
class AVCea608Parser final : public AVSubtitleParser {
public:
  AVCea608Parser();
  explicit AVCea608Parser(int selectedChannel);
  ~AVCea608Parser() override;

  AVCea608Parser(const AVCea608Parser&) = delete;
  AVCea608Parser& operator=(const AVCea608Parser&) = delete;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;

private:
  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CEA608_CEA608_PARSER_H
