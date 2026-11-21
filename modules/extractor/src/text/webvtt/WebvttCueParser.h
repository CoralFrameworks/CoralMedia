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

#ifndef CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CUE_PARSER_H
#define CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CUE_PARSER_H

#include <extractor/text/Cue.h>

#include "text/webvtt/WebvttCss.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coral {
namespace text {
namespace webvtt {

/**
 * Sets the WebVTT defaults of a cue, before its settings list is applied.
 *
 * <p>The defaults are the ones of the specification: the text is centred, the line is a number with
 * the start anchor, the size is the whole viewport and the position is derived from the alignment
 * once the settings were read.
 */
void applyCueDefaults(AVCue& cue);

/**
 * Parses a WebVTT cue timing line, e.g. `00:00:01.000 --> 00:00:03.500 align:start`.
 *
 * @param line The line to parse.
 * @param startUs Set to the start of the cue when the line is a timing line.
 * @param endUs Set to the end of the cue when the line is a timing line.
 * @param settings Set to the settings list, which may be empty.
 *
 * @return {@code false} when the line is not a valid timing line.
 */
bool parseCueHeaderLine(std::string_view line, int64_t& startUs, int64_t& endUs,
                        std::string& settings);

/**
 * Applies a WebVTT cue settings list (`line:`, `align:`, `position:`, `size:`, `vertical:`) to a
 * cue. An invalid setting is skipped rather than failing the cue, which is what the specification
 * asks for and what the reference implementation does.
 */
void applyCueSettings(std::string_view settings, AVCue& cue);

/**
 * Derives the position, the anchor and the size of a cue from the values that were explicitly set,
 * and clamps the line to the viewport.
 */
void finalizeCue(AVCue& cue);

/**
 * Parses the markup of a WebVTT cue into plain text and spans.
 *
 * <p>Supports the tags of the specification that the reference implementation supports: `<b>`,
 * `<i>`, `<u>`, `<s>`, `<c.class>` with the default colours, `<v voice>`, `<lang tag>`, `<ruby>` and
 * `<rt>`. The tags whose content is decoration rather than text (`<ruby>`, `<rt>`, timestamps) are
 * removed from the text, and the attributes they describe are recorded as spans.
 */
void parseCueText(std::string_view markup, AVCue& cue);

/**
 * Parses the markup of a WebVTT cue and applies the rules of its style sheet.
 *
 * <p>The style sheet is the one of the document's `STYLE` blocks. A rule is applied to the element
 * range its selector matches (`tag`, classes, voice and the cue id), and `::cue` applies to the whole
 * cue. A higher specificity wins over a lower one, and a later rule wins at equal specificity, which
 * is the cascade the reference implementation implements.
 *
 * @param markup The markup of the cue.
 * @param cue The cue whose text and spans are built.
 * @param styles The rules of the style sheet, in document order.
 * @param cueId The identifier of the cue, for `::cue(#id)` selectors.
 */
void parseCueText(std::string_view markup, AVCue& cue, const std::vector<CssStyle>& styles,
  const std::string& cueId);

} // namespace webvtt
} // namespace text
} // namespace coral

#endif //CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CUE_PARSER_H
