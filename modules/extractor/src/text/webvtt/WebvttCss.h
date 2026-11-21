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

#ifndef CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CSS_H
#define CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CSS_H

#include <extractor/text/Cue.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coral {
namespace text {
namespace webvtt {

/** The unit of a CSS `font-size` declaration. */
enum class CssFontSizeUnit : uint8_t {
  /** No `font-size` was declared. */
  kUnset,
  /** Pixels, i.e. an absolute size. */
  kPixels,
  /** A multiple of the cue text size. */
  kEm,
  /** A percentage of the cue text size. */
  kPercent,
};

/**
 * One rule of a WebVTT `STYLE` block.
 *
 * <p>The selector forms are the ones of the reference implementation (media3 `WebvttCssParser`):
 * `::cue` for the whole cue and `::cue(tag#id.class1.class2[voice="someone"])` for an element, with
 * every part of the target optional. The declarations are the subset the specification gives a
 * meaning to: colour, background colour, font family/weight/style/size, text decoration,
 * ruby position and text-combine-upright.
 */
class CssStyle final {
public:
  /** The target cue id of `::cue(#id)`, or empty. */
  std::string targetId;
  /** The target element name of `::cue(tag)`, or empty. */
  std::string targetTag;
  /** The target classes of `::cue(.class1.class2)`, or empty. */
  std::vector<std::string> targetClasses;
  /** The target voice of `::cue([voice="..."])`, or empty. */
  std::string targetVoice;

  bool hasColor = false;
  uint32_t color = 0;

  bool hasBackgroundColor = false;
  uint32_t backgroundColor = 0;

  bool hasFontFamily = false;
  std::string fontFamily;

  bool hasBold = false;
  bool bold = false;
  bool hasItalic = false;
  bool italic = false;
  bool hasUnderline = false;
  bool underline = false;
  bool hasLineThrough = false;
  bool lineThrough = false;

  CssFontSizeUnit fontSizeUnit = CssFontSizeUnit::kUnset;
  float fontSize = 0.0f;

  bool hasRubyPosition = false;
  AVCueSpan::RubyPosition rubyPosition = AVCueSpan::RubyPosition::kUnknown;

  bool hasCombineUpright = false;
  bool combineUpright = false;

  /** The position of the rule in the style sheet, used to break equal-specificity ties. */
  size_t order = 0;
};

/** Parses the rules of one `STYLE` block; rules that cannot be parsed are skipped. */
std::vector<CssStyle> parseCssStyles(const std::string& block);

/**
 * @return the specificity of @p style for an element with @p tag, @p classes and @p voice in a cue
 *         whose id is @p cueId, or 0 when the selector does not match.
 *
 * <p>The weights are the ones of media3 `WebvttCssStyle`: an id is the strongest, a voice next, then
 * a class, and the element name is the weakest. A universal `::cue` rule matches only the whole-cue
 * pseudo element, which is the element with an empty name.
 */
int cssSpecificity(const CssStyle& style, const std::string& cueId, const std::string& tag,
  const std::vector<std::string>& classes, const std::string& voice);

} // namespace webvtt
} // namespace text
} // namespace coral

#endif // CORAL_EXTRACTOR_SRC_TEXT_WEBVTT_WEBVTT_CSS_H
