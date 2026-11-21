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

#include <extractor/text/subrip/SubripParser.h>

#include "text/SubtitleTextUtil.h"

#include <media/Definitions.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr float kStartFraction = 0.08f;
constexpr float kEndFraction = 1.0f - kStartFraction;
constexpr float kMiddleFraction = 0.5f;
/** The left inset of one `<blockquote>` level, as a fraction of the viewport width. */
constexpr float kBlockquoteIndent = 0.05f;

struct NamedColor final {
  const char* name;
  uint32_t argb;
};

constexpr NamedColor kNamedColors[] = {
  {"black", 0xFF000000u}, {"white", 0xFFFFFFFFu}, {"red", 0xFFFF0000u},
  {"lime", 0xFF00FF00u},  {"green", 0xFF008000u}, {"blue", 0xFF0000FFu},
  {"yellow", 0xFFFFFF00u}, {"cyan", 0xFF00FFFFu}, {"aqua", 0xFF00FFFFu},
  {"magenta", 0xFFFF00FFu}, {"fuchsia", 0xFFFF00FFu}, {"gray", 0xFF808080u},
  {"grey", 0xFF808080u},  {"silver", 0xFFC0C0C0u}, {"maroon", 0xFF800000u},
  {"olive", 0xFF808000u}, {"navy", 0xFF000080u},  {"purple", 0xFF800080u},
  {"teal", 0xFF008080u},
};

uint32_t parseColorValue(std::string_view value) {
  const std::string trimmed = text::trim(value);
  value = trimmed;
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
    const char quote = value.front();
    const size_t end = value.find(quote, 1);
    value = end == std::string_view::npos ? value.substr(1) : value.substr(1, end - 1);
  }
  const auto hexDigit = [](const char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  };
  if (!value.empty() && value.front() == '#') {
    value = value.substr(1);
    if (value.size() == 3) {
      int digits[3];
      for (size_t i = 0; i < value.size(); ++i) {
        digits[i] = hexDigit(value[i]);
        if (digits[i] < 0) {
          return 0;
        }
      }
      const uint32_t rgb = static_cast<uint32_t>((digits[0] << 4) | digits[0]) << 16 |
                           static_cast<uint32_t>((digits[1] << 4) | digits[1]) << 8 |
                           static_cast<uint32_t>((digits[2] << 4) | digits[2]);
      return 0xFF000000u | rgb;
    }
    if (value.size() == 6) {
      uint32_t rgb = 0;
      for (const char character : value) {
        const int digit = hexDigit(character);
        if (digit < 0) {
          return 0;
        }
        rgb = (rgb << 4) | static_cast<uint32_t>(digit);
      }
      return 0xFF000000u | rgb;
    }
    return 0;
  }
  const std::string lower = text::toLowerAscii(value);
  for (const auto& color : kNamedColors) {
    if (lower == color.name) {
      return color.argb;
    }
  }
  return 0;
}

/** One element of the tag stack of a cue text. */
struct OpenTag final {
  std::string name;
  size_t start = 0;
  uint32_t color = 0;
  std::string fontFamily;
  /** Whether the element is a block (`p`, `div`, `blockquote` or a heading). */
  bool block = false;
  /** 1..6 for an `h1`..`h6`, 0 otherwise. */
  int headingLevel = 0;
};

/** Parses the small HTML subset of a SubRip cue text into plain text and spans. */
class MarkupParser final {
public:
  void parse(const std::string_view markup, AVCue& cue) {
    size_t position = 0;
    while (position < markup.size()) {
      const char character = markup[position];
      if (character == '<') {
        const size_t end = markup.find('>', position + 1);
        if (end == std::string_view::npos) {
          mText.push_back(character);
          ++position;
          continue;
        }
        handleTag(text::trim(markup.substr(position + 1, end - position - 1)));
        position = end + 1;
      } else if (character == '&') {
        position = handleEntity(markup, position);
      } else {
        mText.push_back(character);
        ++position;
      }
    }
    while (!mStack.empty()) {
      applyTag(mStack.back(), mText.size());
      mStack.pop_back();
    }
    cue.paragraphIndent = std::min(0.5f, kBlockquoteIndent * static_cast<float>(mMaxBlockquoteDepth));
    cue.text = std::move(mText);
    cue.spans = std::move(mSpans);
  }

private:
  void handleTag(const std::string& tag) {
    if (tag.empty()) {
      return;
    }
    if (tag.front() == '/') {
      const std::string name = text::toLowerAscii(text::trim(std::string_view(tag).substr(1)));
      closeTag(name);
      return;
    }
    // `<br>` and `<br/>` are line breaks rather than elements.
    std::string namePart = tag;
    const size_t space = namePart.find_first_of(" \t/");
    std::string attributes;
    if (space != std::string::npos) {
      attributes = namePart.substr(space + 1);
      namePart = namePart.substr(0, space);
    }
    const std::string name = text::toLowerAscii(text::trim(namePart));
    if (name == "br") {
      mText.push_back('\n');
      return;
    }
    if (name.empty()) {
      return;
    }
    const bool heading = name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
    const bool block = heading || name == "p" || name == "div" || name == "blockquote";
    if (name == "blockquote") {
      ++mBlockquoteDepth;
      mMaxBlockquoteDepth = std::max(mMaxBlockquoteDepth, mBlockquoteDepth);
    }
    if (block) {
      paragraphBreak();
    }
    OpenTag open;
    open.name = name;
    open.start = mText.size();
    open.block = block;
    open.headingLevel = heading ? name[1] - '0' : 0;
    if (name == "font") {
      open.color = parseColorValue(attributeValue(attributes, "color"));
      open.fontFamily = attributeValue(attributes, "face");
    }
    mStack.push_back(std::move(open));
  }

  /**
   * Inserts the paragraph break of a block element.
   *
   * <p>This is the rule of the Android HTML parser: an empty line is the most a paragraph break
   * adds, so `<p>a</p><p>b</p>` becomes `a\n\nb` and a break never creates three newlines in a row.
   */
  void paragraphBreak() {
    const size_t length = mText.size();
    if (length >= 1 && mText.back() == '\n') {
      if (length >= 2 && mText[length - 2] == '\n') {
        return;
      }
      mText.push_back('\n');
      return;
    }
    if (length != 0) {
      mText.append("\n\n");
    }
  }

  /**
   * @return the value of @p name in the attribute list of a tag, or an empty string.
   *
   * <p>The value may be quoted, and the comparison of the name is case insensitive, because
   * SubRip files are written by many tools and `<FONT FACE="Arial">` is as valid as the lower case
   * form.
   */
  static std::string attributeValue(const std::string_view attributes, const std::string_view name) {
    const std::string lower = text::toLowerAscii(attributes);
    size_t position = 0;
    while ((position = lower.find(name, position)) != std::string::npos) {
      const size_t equals = attributes.find('=', position + name.size());
      if (equals == std::string_view::npos) {
        return {};
      }
      std::string_view value = attributes.substr(equals + 1);
      size_t begin = 0;
      while (begin < value.size() &&
             (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' ||
               value[begin] == '\r')) {
        ++begin;
      }
      if (begin < value.size() && (value[begin] == '"' || value[begin] == '\'')) {
        const char quote = value[begin];
        const size_t end = value.find(quote, begin + 1);
        if (end == std::string_view::npos) {
          return std::string(value.substr(begin + 1));
        }
        return std::string(value.substr(begin + 1, end - begin - 1));
      }
      size_t end = begin;
      while (end < value.size() && value[end] != ' ' && value[end] != '\t' &&
             value[end] != '\n' && value[end] != '\r' && value[end] != '>') {
        ++end;
      }
      return std::string(value.substr(begin, end - begin));
    }
    return {};
  }

  void closeTag(const std::string& name) {
    for (auto it = mStack.rbegin(); it != mStack.rend(); ++it) {
      if (it->name == name) {
        while (!mStack.empty()) {
          OpenTag tag = std::move(mStack.back());
          mStack.pop_back();
          applyTag(tag, mText.size());
          if (tag.name == name) {
            return;
          }
        }
        return;
      }
    }
  }

  void applyTag(const OpenTag& tag, const size_t end) {
    if (end <= tag.start) {
      if (tag.block) {
        paragraphBreak();
      }
      return;
    }
    if (tag.name == "font") {
      if (tag.color != 0) {
        AVCueSpan color;
        color.type = AVCueSpan::Type::kForegroundColor;
        color.start = tag.start;
        color.end = end;
        color.color = tag.color;
        mSpans.push_back(std::move(color));
      }
      if (!tag.fontFamily.empty()) {
        AVCueSpan family;
        family.type = AVCueSpan::Type::kTextStyle;
        family.start = tag.start;
        family.end = end;
        family.style.fontFamily = tag.fontFamily;
        mSpans.push_back(std::move(family));
      }
    } else {
      static constexpr float kHeadingSizes[] = {1.5f, 1.4f, 1.3f, 1.2f, 1.1f, 1.0f};
      AVCueSpan span;
      span.start = tag.start;
      span.end = end;
      span.type = AVCueSpan::Type::kTextStyle;
      bool known = true;
      if (tag.name == "b" || tag.name == "strong") {
        span.style.bold = true;
      } else if (tag.name == "i" || tag.name == "em" || tag.name == "cite" ||
                 tag.name == "dfn") {
        span.style.italic = true;
      } else if (tag.name == "u") {
        span.style.underline = true;
      } else if (tag.name == "s" || tag.name == "strike" || tag.name == "del") {
        span.style.strikeout = true;
      } else if (tag.name == "big") {
        span.style.textSize = 1.25f;
      } else if (tag.name == "small") {
        span.style.textSize = 0.8f;
      } else if (tag.name == "tt") {
        span.style.fontFamily = "monospace";
      } else if (tag.headingLevel >= 1 && tag.headingLevel <= 6) {
        span.style.bold = true;
        span.style.textSize = kHeadingSizes[tag.headingLevel - 1];
      } else if (tag.name == "sub") {
        span.type = AVCueSpan::Type::kSubscript;
      } else if (tag.name == "sup") {
        span.type = AVCueSpan::Type::kSuperscript;
      } else {
        known = false;
      }
      if (known) {
        mSpans.push_back(std::move(span));
      }
    }
    if (tag.block) {
      paragraphBreak();
    }
    if (tag.name == "blockquote" && mBlockquoteDepth > 0) {
      --mBlockquoteDepth;
    }
  }

  size_t handleEntity(const std::string_view markup, const size_t position) {
    const size_t semicolon = markup.find(';', position + 1);
    if (semicolon == std::string_view::npos || semicolon - position > 10) {
      mText.push_back('&');
      return position + 1;
    }
    const std::string_view entity = markup.substr(position + 1, semicolon - position - 1);
    if (entity == "amp") {
      mText.push_back('&');
    } else if (entity == "lt") {
      mText.push_back('<');
    } else if (entity == "gt") {
      mText.push_back('>');
    } else if (entity == "quot") {
      mText.push_back('"');
    } else if (entity == "apos") {
      mText.push_back('\'');
    } else if (entity == "nbsp") {
      mText.push_back(' ');
    } else if (!entity.empty() && entity.front() == '#') {
      appendNumericEntity(entity);
    } else {
      // An entity that is not known is kept as it was written rather than dropped, because the
      // text around it is still meaningful.
      mText.append(markup.substr(position, semicolon - position + 1));
    }
    return semicolon + 1;
  }

  void appendNumericEntity(const std::string_view entity) {
    const bool hexadecimal = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
    const std::string_view digits = entity.substr(hexadecimal ? 2 : 1);
    int64_t value = 0;
    if (digits.empty()) {
      return;
    }
    for (const char character : digits) {
      const bool digit = character >= '0' && character <= '9';
      const bool hexDigit = (character >= 'a' && character <= 'f') ||
                            (character >= 'A' && character <= 'F');
      if (!digit && !(hexadecimal && hexDigit)) {
        return;
      }
      value = value * (hexadecimal ? 16 : 10) +
              (digit ? character - '0' : (character | 0x20) - 'a' + 10);
      if (value > 0x10FFFF) {
        return;
      }
    }
    appendUtf8(mText, static_cast<uint32_t>(value));
  }

  static void appendUtf8(std::string& out, const uint32_t codePoint) {
    if (codePoint <= 0x7F) {
      out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
  }

  std::string mText;
  std::vector<AVCueSpan> mSpans;
  int mBlockquoteDepth = 0;
  int mMaxBlockquoteDepth = 0;
  std::vector<OpenTag> mStack;
};

/**
 * Removes the legacy `{\...}` tags from a line.
 *
 * @param line The line to clean.
 * @param alignment Set to the last `\anN` value that the line held, or 0 when it held none.
 *
 * @return The text of the line without its tags.
 */
std::string stripLegacyTags(const std::string_view line, int& alignment) {
  std::string cleaned;
  size_t position = 0;
  while (position < line.size()) {
    if (line[position] == '{') {
      const size_t end = line.find('}', position + 1);
      if (end != std::string_view::npos) {
        const std::string_view block = line.substr(position + 1, end - position - 1);
        if (!block.empty() && block.front() == '\\') {
          size_t search = 0;
          while ((search = block.find("\\an", search)) != std::string_view::npos) {
            const size_t digitPosition = search + 3;
            if (digitPosition < block.size() && block[digitPosition] >= '1' &&
                block[digitPosition] <= '9') {
              alignment = block[digitPosition] - '0';
            }
            search = digitPosition;
          }
          position = end + 1;
          continue;
        }
      }
    }
    cleaned.push_back(line[position]);
    ++position;
  }
  return text::trim(cleaned);
}

float fractionForAnchor(const AVCue::AnchorType anchor) {
  switch (anchor) {
    case AVCue::AnchorType::kStart:
      return kStartFraction;
    case AVCue::AnchorType::kMiddle:
      return kMiddleFraction;
    case AVCue::AnchorType::kEnd:
      return kEndFraction;
    case AVCue::AnchorType::kUnset:
    default:
      return AVCue::kDimenUnset;
  }
}

/** Applies the alignment of an `{\anN}` tag to @p cue. */
void applyAlignment(const int alignment, AVCue& cue) {
  if (alignment < 1 || alignment > 9) {
    return;
  }
  AVCue::AnchorType positionAnchor = AVCue::AnchorType::kMiddle;
  if (alignment % 3 == 1) {
    positionAnchor = AVCue::AnchorType::kStart;
  } else if (alignment % 3 == 0) {
    positionAnchor = AVCue::AnchorType::kEnd;
  }
  AVCue::AnchorType lineAnchor = AVCue::AnchorType::kMiddle;
  if (alignment <= 3) {
    lineAnchor = AVCue::AnchorType::kEnd;
  } else if (alignment >= 7) {
    lineAnchor = AVCue::AnchorType::kStart;
  }
  cue.positionAnchor = positionAnchor;
  cue.lineAnchor = lineAnchor;
  cue.position = fractionForAnchor(positionAnchor);
  cue.line = fractionForAnchor(lineAnchor);
  cue.lineType = AVCue::LineType::kFraction;
}

/** Parses a timing line of the form `start --> end [position]`. */
bool parseTimingLine(const std::string_view line, int64_t& startUs, int64_t& endUs) {
  const size_t arrow = line.find("-->");
  if (arrow == std::string_view::npos) {
    return false;
  }
  if (!text::parseSubripTimestamp(line.substr(0, arrow), startUs)) {
    return false;
  }
  const std::string rest = text::trim(line.substr(arrow + 3));
  const size_t space = rest.find_first_of(" \t");
  const std::string_view endText =
    space == std::string::npos ? std::string_view(rest) : std::string_view(rest).substr(0, space);
  return text::parseSubripTimestamp(endText, endUs) && endUs >= startUs;
}

} // namespace

bool AVSubripParser::parse(const uint8_t* data, const size_t size, const OutputOptions& options,
                           const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) {
    return false;
  }
  text::Reader reader(data, size);
  std::vector<AVCuesWithTiming> results;
  std::string line;
  while (reader.readLine(line)) {
    if (line.empty()) {
      continue;
    }
    if (line.find("-->") == std::string::npos) {
      // The index of a cue, or a line that is not part of the structure of the format.
      continue;
    }
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimingLine(line, startUs, endUs)) {
      continue;
    }

    int alignment = 0;
    std::string markup;
    std::string textLine;
    while (reader.readLine(textLine)) {
      if (textLine.empty()) {
        break;
      }
      const std::string cleaned = stripLegacyTags(textLine, alignment);
      if (!markup.empty()) {
        markup.push_back('\n');
      }
      markup += cleaned;
    }

    AVCue cue;
    MarkupParser markupParser;
    markupParser.parse(markup, cue);
    applyAlignment(alignment, cue);

    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = startUs;
    result.durationUs = endUs - startUs;
    results.push_back(std::move(result));
  }

  text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues, output);
  return true;
}

void AVSubripParser::reset() {
  // The parser holds no state across samples: every call is a whole document.
}

} // namespace coral
