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

#ifndef CORAL_EXTRACTOR_TEXT_DEFAULT_SUBTITLE_PARSER_FACTORY_H
#define CORAL_EXTRACTOR_TEXT_DEFAULT_SUBTITLE_PARSER_FACTORY_H

#include <extractor/text/SubtitleParser.h>

#include <memory>

namespace coral {

/**
 * The factory of the subtitle parsers that the extractor module ships.
 *
 * <p>This is the counterpart of ExoPlayer's {@code DefaultSubtitleParserFactory}: it maps the sample
 * MIME type of a {@link MediaFormat} to the parser of that format. The parsers are registered one
 * format at a time, so a build that trims the formats it does not need stays possible.
 */
class AVDefaultSubtitleParserFactory final : public AVSubtitleParserFactory {
public:
  AVDefaultSubtitleParserFactory() = default;

  ~AVDefaultSubtitleParserFactory() override = default;

  [[nodiscard]] bool supportsFormat(const MediaFormat& format) const override;

  [[nodiscard]] CueReplacementBehavior
  getCueReplacementBehavior(const MediaFormat& format) const override;

  [[nodiscard]] std::unique_ptr<AVSubtitleParser> create(const MediaFormat& format) const override;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_DEFAULT_SUBTITLE_PARSER_FACTORY_H
