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

#include <extractor/text/ssa/SsaParser.h>

#include "text/SubtitleTextUtil.h"

#include <media/Definitions.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr size_t kIndexUnset = std::numeric_limits<size_t>::max();
constexpr float kDefaultMargin = 0.05f;

/** The indices of the properties of a `Style:` line. */
struct StyleFormat final {
  size_t name = kIndexUnset;
  size_t alignment = kIndexUnset;
  size_t primaryColor = kIndexUnset;
  size_t outlineColor = kIndexUnset;
  size_t fontSize = kIndexUnset;
  size_t bold = kIndexUnset;
  size_t italic = kIndexUnset;
  size_t underline = kIndexUnset;
  size_t strikeout = kIndexUnset;
  size_t borderStyle = kIndexUnset;
  size_t length = 0;
  bool valid = false;
};

/** One row of a `[V4+ Styles]` section. */
struct Style final {
  std::string name;
  int alignment = 0;
  uint32_t primaryColor = 0;
  bool hasPrimaryColor = false;
  uint32_t outlineColor = 0;
  bool hasOutlineColor = false;
  float fontSize = AVCue::kDimenUnset;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikeout = false;
  bool boxBorder = false;
};

/** The indices of the properties of a `Dialogue:` line. */
struct DialogueFormat final {
  size_t layer = kIndexUnset;
  size_t start = kIndexUnset;
  size_t end = kIndexUnset;
  size_t style = kIndexUnset;
  size_t text = kIndexUnset;
  size_t length = 0;
  bool valid = false;
};

/** The style attributes of a run of dialogue text. */
struct RunStyle final {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikeout = false;
  uint32_t color = 0;
  bool hasColor = false;
};

/** The placement overrides of a dialogue line. */
struct Overrides final {
  int alignment = 0;
  bool hasPosition = false;
  float x = 0.0f;
  float y = 0.0f;
};

/** A dialogue line with the cue it describes. */
struct ParsedDialogue final {
  int64_t startUs = 0;
  int64_t endUs = 0;
  AVCue cue;
};

/** Parses a `Format:` line of the `[V4+ Styles]` section. */
StyleFormat parseStyleFormat(const std::string_view line) {
  StyleFormat format;
  if (!text::startsWithIgnoreCase(line, "Format:")) {
    return format;
  }
  const std::vector<std::string> keys = text::split(std::string_view(line).substr(7), ',');
  format.length = keys.size();
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string key = text::toLowerAscii(text::trim(keys[i]));
    if (key == "name") {
      format.name = i;
    } else if (key == "alignment") {
      format.alignment = i;
    } else if (key == "primarycolour" || key == "primarycolor") {
      format.primaryColor = i;
    } else if (key == "outlinecolour" || key == "outlinecolor") {
      format.outlineColor = i;
    } else if (key == "fontsize") {
      format.fontSize = i;
    } else if (key == "bold") {
      format.bold = i;
    } else if (key == "italic") {
      format.italic = i;
    } else if (key == "underline") {
      format.underline = i;
    } else if (key == "strikeout") {
      format.strikeout = i;
    } else if (key == "borderstyle") {
      format.borderStyle = i;
    }
  }
  format.valid = format.name != kIndexUnset && format.length > 0;
  return format;
}

/** Parses a `Format:` line of the `[Events]` section. */
DialogueFormat parseDialogueFormat(const std::string_view line) {
  DialogueFormat format;
  if (!text::startsWithIgnoreCase(line, "Format:")) {
    return format;
  }
  const std::vector<std::string> keys = text::split(std::string_view(line).substr(7), ',');
  format.length = keys.size();
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string key = text::toLowerAscii(text::trim(keys[i]));
    if (key == "layer") {
      format.layer = i;
    } else if (key == "start") {
      format.start = i;
    } else if (key == "end") {
      format.end = i;
    } else if (key == "style") {
      format.style = i;
    } else if (key == "text") {
      format.text = i;
    }
  }
  format.valid = format.start != kIndexUnset && format.end != kIndexUnset &&
                 format.text != kIndexUnset;
  return format;
}

/** Parses an SSA alignment (the points of a number pad); 0 means unknown. */
int parseAlignment(const std::string_view value) {
  int64_t alignment = 0;
  if (text::parseInteger(text::trim(value), alignment) && alignment >= 1 && alignment <= 9) {
    return static_cast<int>(alignment);
  }
  return 0;
}

/** Parses an SSA colour (`&HAABBGGRR` or a decimal value) into ARGB, as ExoPlayer does. */
bool parseSsaColor(const std::string_view expression, uint32_t& argb) {
  std::string value = text::trim(expression);
  if (value.empty()) {
    return false;
  }
  uint64_t abgr = 0;
  if (value.size() > 2 && value[0] == '&' && (value[1] == 'H' || value[1] == 'h')) {
    std::string digits = value.substr(2);
    // An inline colour is written with a closing ampersand, a style row without one.
    while (!digits.empty() && digits.back() == '&') {
      digits.pop_back();
    }
    if (digits.empty() || digits.size() > 8) {
      return false;
    }
    for (const char character : digits) {
      int digit = -1;
      if (character >= '0' && character <= '9') {
        digit = character - '0';
      } else if (character >= 'a' && character <= 'f') {
        digit = character - 'a' + 10;
      } else if (character >= 'A' && character <= 'F') {
        digit = character - 'A' + 10;
      } else {
        return false;
      }
      abgr = (abgr << 4) | static_cast<uint64_t>(digit);
    }
  } else {
    int64_t parsed = 0;
    if (!text::parseInteger(value, parsed) || parsed < 0) {
      return false;
    }
    abgr = static_cast<uint64_t>(parsed);
  }
  if (abgr > 0xFFFFFFFFull) {
    return false;
  }
  // SSA stores the alpha channel inverted: 0 means opaque and 0xFF means transparent.
  const auto alpha = static_cast<uint32_t>(((abgr >> 24) & 0xFFull) ^ 0xFFull);
  const auto blue = static_cast<uint32_t>((abgr >> 16) & 0xFFull);
  const auto green = static_cast<uint32_t>((abgr >> 8) & 0xFFull);
  const auto red = static_cast<uint32_t>(abgr & 0xFFull);
  argb = (alpha << 24) | (red << 16) | (green << 8) | blue;
  return true;
}

/** Parses the `1` and `-1` of an SSA boolean expression. */
bool parseSsaBoolean(const std::string_view value, bool& out) {
  int64_t parsed = 0;
  if (!text::parseInteger(text::trim(value), parsed)) {
    return false;
  }
  out = parsed == 1 || parsed == -1;
  return true;
}

/** Parses a `Style:` line into @p style. */
bool parseStyle(const std::string_view line, const StyleFormat& format, Style& style) {
  if (!format.valid || !text::startsWithIgnoreCase(line, "Style:")) {
    return false;
  }
  const std::vector<std::string> values =
    text::split(std::string_view(line).substr(6), ',', format.length);
  if (values.size() != format.length) {
    return false;
  }
  style.name = text::trim(values[format.name]);
  if (style.name.empty()) {
    return false;
  }
  if (format.alignment != kIndexUnset) {
    style.alignment = parseAlignment(values[format.alignment]);
  }
  if (format.primaryColor != kIndexUnset) {
    style.hasPrimaryColor = parseSsaColor(values[format.primaryColor], style.primaryColor);
  }
  if (format.outlineColor != kIndexUnset) {
    style.hasOutlineColor = parseSsaColor(values[format.outlineColor], style.outlineColor);
  }
  if (format.fontSize != kIndexUnset) {
    float size = 0.0f;
    if (text::parseFloat(text::trim(values[format.fontSize]), size) && size > 0.0f) {
      style.fontSize = size;
    }
  }
  if (format.bold != kIndexUnset) {
    parseSsaBoolean(values[format.bold], style.bold);
  }
  if (format.italic != kIndexUnset) {
    parseSsaBoolean(values[format.italic], style.italic);
  }
  if (format.underline != kIndexUnset) {
    parseSsaBoolean(values[format.underline], style.underline);
  }
  if (format.strikeout != kIndexUnset) {
    parseSsaBoolean(values[format.strikeout], style.strikeout);
  }
  if (format.borderStyle != kIndexUnset) {
    int64_t borderStyle = 0;
    if (text::parseInteger(text::trim(values[format.borderStyle]), borderStyle)) {
      style.boxBorder = borderStyle == 3;
    }
  }
  return true;
}

float defaultFractionForAnchor(const AVCue::AnchorType anchor) {
  switch (anchor) {
    case AVCue::AnchorType::kStart:
      return kDefaultMargin;
    case AVCue::AnchorType::kMiddle:
      return 0.5f;
    case AVCue::AnchorType::kEnd:
      return 1.0f - kDefaultMargin;
    case AVCue::AnchorType::kUnset:
    default:
      return AVCue::kDimenUnset;
  }
}

/** Reads the two or four arguments of `\pos(...)` / `\move(...)` after the parenthesis. */
bool parsePositionArguments(const std::string_view block, const std::string_view marker,
                            float& x, float& y) {
  const size_t markerPosition = block.find(marker);
  if (markerPosition == std::string_view::npos) {
    return false;
  }
  const size_t open = block.find('(', markerPosition);
  const size_t close = block.find(')', open == std::string_view::npos ? markerPosition : open);
  if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
    return false;
  }
  const std::vector<std::string> arguments =
    text::split(block.substr(open + 1, close - open - 1), ',');
  if (arguments.size() < 2) {
    return false;
  }
  // `\move` states the start and the end point: the end point is where the cue is placed.
  const std::string& first = arguments[arguments.size() - 2];
  const std::string& second = arguments[arguments.size() - 1];
  float parsedX = 0.0f;
  float parsedY = 0.0f;
  if (!text::parseFloat(text::trim(first), parsedX) ||
      !text::parseFloat(text::trim(second), parsedY)) {
    return false;
  }
  x = parsedX;
  y = parsedY;
  return true;
}

/** The text of a cue, with the spans that the inline overrides of a dialogue line describe. */
class TextBuilder final {
public:
  explicit TextBuilder(const RunStyle& initial) : mCurrent(initial) {}

  [[nodiscard]] const RunStyle& currentStyle() const { return mCurrent; }

  void append(const char character) { mText.push_back(character); }

  void appendRaw(const std::string_view value) { mText.append(value.data(), value.size()); }

  void changeStyle(const RunStyle& style) {
    flush();
    mCurrent = style;
  }

  void finish() { flush(); }

  [[nodiscard]] std::string takeText() { return std::move(mText); }

  [[nodiscard]] std::vector<AVCueSpan> takeSpans() { return std::move(mSpans); }

private:
  void flush() {
    if (mText.size() <= mRunStart) {
      mRunStart = mText.size();
      return;
    }
    const size_t end = mText.size();
    if (mCurrent.hasColor) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kForegroundColor;
      span.start = mRunStart;
      span.end = end;
      span.color = mCurrent.color;
      mSpans.push_back(std::move(span));
    }
    if (mCurrent.bold || mCurrent.italic || mCurrent.underline || mCurrent.strikeout) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kTextStyle;
      span.start = mRunStart;
      span.end = end;
      span.style.bold = mCurrent.bold;
      span.style.italic = mCurrent.italic;
      span.style.underline = mCurrent.underline;
      span.style.strikeout = mCurrent.strikeout;
      mSpans.push_back(std::move(span));
    }
    mRunStart = end;
  }

  std::string mText;
  std::vector<AVCueSpan> mSpans;
  RunStyle mCurrent;
  size_t mRunStart = 0;
};

/** Applies the directives of one `{...}` block to the style of the following run. */
void applyInlineOverrides(const std::string_view block, const RunStyle& base, RunStyle& style,
                          Overrides& overrides) {
  size_t position = 0;
  while (position < block.size()) {
    const size_t marker = block.find('\\', position);
    if (marker == std::string_view::npos) {
      break;
    }
    const size_t next = block.find('\\', marker + 1);
    const std::string_view directive =
      block.substr(marker + 1, (next == std::string_view::npos ? block.size() : next) - marker - 1);
    position = next == std::string_view::npos ? block.size() : next;

    if (directive == "r") {
      style = base;
      continue;
    }
    if (directive.rfind("an", 0) == 0 && directive.size() == 3 && directive[2] >= '1' &&
        directive[2] <= '9') {
      overrides.alignment = directive[2] - '0';
      continue;
    }
    if (directive.rfind("pos(", 0) == 0 || directive.rfind("move(", 0) == 0) {
      float x = 0.0f;
      float y = 0.0f;
      if (parsePositionArguments(block, directive.rfind("pos(", 0) == 0 ? "\\pos(" : "\\move(", x,
                                 y)) {
        overrides.hasPosition = true;
        overrides.x = x;
        overrides.y = y;
      }
      continue;
    }
    // The colour directives are `\c`, `\1c`, `\2c`, ... and state a colour expression.
    size_t namePosition = 0;
    while (namePosition + 1 < directive.size() && directive[namePosition] >= '0' &&
           directive[namePosition] <= '9') {
      ++namePosition;
    }
    if (namePosition < directive.size() && directive[namePosition] == 'c') {
      const std::string_view expression = directive.substr(namePosition + 1);
      uint32_t color = 0;
      if (parseSsaColor(expression, color)) {
        style.color = color;
        style.hasColor = true;
      }
      continue;
    }
    if (directive.rfind("alpha", 0) == 0) {
      // An inline alpha is a transparency amount: 0 is opaque, 0xFF is invisible. It is written in
      // hexadecimal after an `H`, e.g. `\alpha&H80&`.
      const size_t ampersand = directive.find('&');
      const size_t end =
        directive.find('&', ampersand == std::string_view::npos ? 0 : ampersand + 1);
      if (ampersand != std::string_view::npos && end != std::string_view::npos &&
          end > ampersand + 1) {
        std::string digits(directive.substr(ampersand + 1, end - ampersand - 1));
        if (!digits.empty() && (digits[0] == 'H' || digits[0] == 'h')) {
          digits.erase(0, 1);
        }
        uint32_t transparency = 0;
        bool valid = !digits.empty() && digits.size() <= 2;
        for (const char character : digits) {
          const int digit = character >= '0' && character <= '9'
                              ? character - '0'
                              : (character >= 'a' && character <= 'f'
                                   ? character - 'a' + 10
                                   : (character >= 'A' && character <= 'F' ? character - 'A' + 10
                                                                           : -1));
          if (digit < 0) {
            valid = false;
            break;
          }
          transparency = (transparency << 4) | static_cast<uint32_t>(digit);
        }
        if (valid) {
          const uint32_t alpha = 255u - std::min<uint32_t>(transparency, 255u);
          if (style.hasColor) {
            style.color = (style.color & 0x00FFFFFFu) | (alpha << 24);
          }
        }
      }
      continue;
    }
    // The single letter directives of the bold, italic, underline and strikeout attributes.
    if (directive.size() >= 2) {
      const char name = directive[0];
      if (name == 'b' || name == 'i' || name == 'u' || name == 's') {
        bool enabled = false;
        if (parseSsaBoolean(directive.substr(1), enabled)) {
          if (name == 'b') {
            style.bold = enabled;
          } else if (name == 'i') {
            style.italic = enabled;
          } else if (name == 'u') {
            style.underline = enabled;
          } else {
            style.strikeout = enabled;
          }
        }
      }
    }
  }
}

AVCue::AnchorType lineAnchorForAlignment(const int alignment) {
  if (alignment >= 1 && alignment <= 3) {
    return AVCue::AnchorType::kEnd;
  }
  if (alignment >= 4 && alignment <= 6) {
    return AVCue::AnchorType::kMiddle;
  }
  if (alignment >= 7 && alignment <= 9) {
    return AVCue::AnchorType::kStart;
  }
  return AVCue::AnchorType::kUnset;
}

AVCue::AnchorType positionAnchorForAlignment(const int alignment) {
  if (alignment <= 0 || alignment > 9) {
    return AVCue::AnchorType::kUnset;
  }
  switch (alignment % 3) {
    case 1:
      return AVCue::AnchorType::kStart;
    case 2:
      return AVCue::AnchorType::kMiddle;
    default:
      return AVCue::AnchorType::kEnd;
  }
}

AVCue::Alignment textAlignmentForAlignment(const int alignment) {
  if (alignment <= 0 || alignment > 9) {
    return AVCue::Alignment::kUnset;
  }
  switch (alignment % 3) {
    case 1:
      return AVCue::Alignment::kLeft;
    case 2:
      return AVCue::Alignment::kCenter;
    default:
      return AVCue::Alignment::kRight;
  }
}

/** Builds the cue of a dialogue line. */
ParsedDialogue buildDialogue(const std::string_view rawText, const int layer, const Style* style,
                             const float screenWidth, const float screenHeight) {
  ParsedDialogue result;
  RunStyle base;
  if (style != nullptr) {
    base.bold = style->bold;
    base.italic = style->italic;
    base.underline = style->underline;
    base.strikeout = style->strikeout;
    base.hasColor = style->hasPrimaryColor;
    base.color = style->primaryColor;
  }

  TextBuilder builder(base);
  Overrides overrides;
  size_t position = 0;
  while (position < rawText.size()) {
    const char character = rawText[position];
    if (character == '{') {
      const size_t end = rawText.find('}', position + 1);
      if (end != std::string_view::npos) {
        const std::string_view block = rawText.substr(position + 1, end - position - 1);
        if (!block.empty() && block.front() == '\\') {
          RunStyle next = builder.currentStyle();
          applyInlineOverrides(block, base, next, overrides);
          builder.changeStyle(next);
          position = end + 1;
          continue;
        }
      }
      builder.append(character);
      ++position;
      continue;
    }
    if (character == '\\' && position + 1 < rawText.size()) {
      const char escaped = rawText[position + 1];
      if (escaped == 'N' || escaped == 'n') {
        builder.append('\n');
        position += 2;
        continue;
      }
      if (escaped == 'h') {
        builder.appendRaw("\xC2\xA0");
        position += 2;
        continue;
      }
    }
    builder.append(character);
    ++position;
  }
  builder.finish();

  AVCue& cue = result.cue;
  cue.text = builder.takeText();
  cue.spans = builder.takeSpans();
  cue.zIndex = layer;

  if (style != nullptr) {
    if (style->boxBorder && style->hasOutlineColor && !cue.text.empty()) {
      AVCueSpan background;
      background.type = AVCueSpan::Type::kBackgroundColor;
      background.start = 0;
      background.end = cue.text.size();
      background.color = style->outlineColor;
      cue.spans.push_back(std::move(background));
    }
    if (style->fontSize != AVCue::kDimenUnset && screenHeight != AVCue::kDimenUnset) {
      cue.textSize = style->fontSize / screenHeight;
      cue.textSizeType = AVCue::TextSizeType::kFractionalIgnorePadding;
    }
  }

  int alignment = overrides.alignment;
  if (alignment == 0 && style != nullptr) {
    alignment = style->alignment;
  }
  cue.textAlignment = textAlignmentForAlignment(alignment);
  cue.positionAnchor = positionAnchorForAlignment(alignment);
  cue.lineAnchor = lineAnchorForAlignment(alignment);
  if (overrides.hasPosition && screenWidth != AVCue::kDimenUnset &&
      screenHeight != AVCue::kDimenUnset) {
    cue.position = overrides.x / screenWidth;
    cue.line = overrides.y / screenHeight;
    cue.lineType = AVCue::LineType::kFraction;
  } else {
    cue.position = defaultFractionForAnchor(cue.positionAnchor);
    const float line = defaultFractionForAnchor(cue.lineAnchor);
    if (line != AVCue::kDimenUnset) {
      cue.line = line;
      cue.lineType = AVCue::LineType::kFraction;
    }
  }
  return result;
}

/** Parses the `[Script Info]` and `[V4+ Styles]` sections of a header. */
void parseHeaderSections(text::Reader& reader, bool& haveStyles,
                         std::map<std::string, Style>& styles, float& screenWidth,
                         float& screenHeight) {
  enum class Section : uint8_t { kNone, kScriptInfo, kStyles };
  Section section = Section::kNone;
  StyleFormat styleFormat;
  bool haveStyleFormat = false;
  std::string line;
  while (reader.readLine(line)) {
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[') {
      const std::string lowered = text::toLowerAscii(text::trim(line));
      if (lowered == "[events]") {
        return;
      }
      if (lowered == "[script info]") {
        section = Section::kScriptInfo;
      } else if (lowered == "[v4+ styles]") {
        section = Section::kStyles;
        haveStyles = true;
      } else {
        section = Section::kNone;
      }
      continue;
    }
    if (section == Section::kScriptInfo) {
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string name = text::toLowerAscii(text::trim(std::string_view(line).substr(0, colon)));
      float value = 0.0f;
      if (name == "playresx") {
        if (text::parseFloat(text::trim(std::string_view(line).substr(colon + 1)), value) &&
            value > 0.0f) {
          screenWidth = value;
        }
      } else if (name == "playresy") {
        if (text::parseFloat(text::trim(std::string_view(line).substr(colon + 1)), value) &&
            value > 0.0f) {
          screenHeight = value;
        }
      }
    } else if (section == Section::kStyles) {
      if (text::startsWithIgnoreCase(line, "Format:")) {
        styleFormat = parseStyleFormat(line);
        haveStyleFormat = styleFormat.valid;
      } else if (text::startsWithIgnoreCase(line, "Style:") && haveStyleFormat) {
        Style style;
        if (parseStyle(line, styleFormat, style)) {
          styles[style.name] = std::move(style);
        }
      }
    }
  }
}

} // namespace

struct AVSsaParser::Impl final {
  bool haveInitializationData = false;
  bool hasInitializationDialogueFormat = false;
  DialogueFormat initializationDialogueFormat;
  bool haveStyles = false;
  std::map<std::string, Style> styles;
  float screenWidth = AVCue::kDimenUnset;
  float screenHeight = AVCue::kDimenUnset;
};

AVSsaParser::AVSsaParser() : mImpl(new Impl()) {
}

AVSsaParser::AVSsaParser(const std::vector<std::vector<uint8_t>>& initializationData)
  : mImpl(new Impl()) {
  if (initializationData.size() < 2 || initializationData[0].empty()) {
    return;
  }
  const std::string formatLine(reinterpret_cast<const char*>(initializationData[0].data()),
                               initializationData[0].size());
  if (!text::startsWithIgnoreCase(formatLine, "Format:")) {
    return;
  }
  DialogueFormat format = parseDialogueFormat(formatLine);
  if (!format.valid) {
    return;
  }
  mImpl->haveInitializationData = true;
  mImpl->hasInitializationDialogueFormat = true;
  mImpl->initializationDialogueFormat = format;

  const std::vector<uint8_t>& header = initializationData[1];
  text::Reader reader(header.data(), header.size());
  parseHeaderSections(reader, mImpl->haveStyles, mImpl->styles, mImpl->screenWidth,
                      mImpl->screenHeight);
}

AVSsaParser::~AVSsaParser() = default;

bool AVSsaParser::parse(const uint8_t* data, const size_t size, const OutputOptions& options,
                        const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) {
    return false;
  }
  Impl& impl = *mImpl;
  text::Reader reader(data, size);
  if (!impl.haveInitializationData) {
    impl.styles.clear();
    impl.haveStyles = false;
    impl.screenWidth = AVCue::kDimenUnset;
    impl.screenHeight = AVCue::kDimenUnset;
    parseHeaderSections(reader, impl.haveStyles, impl.styles, impl.screenWidth,
                        impl.screenHeight);
  }

  DialogueFormat format =
    impl.hasInitializationDialogueFormat ? impl.initializationDialogueFormat : DialogueFormat{};
  std::vector<ParsedDialogue> dialogues;
  std::string line;
  while (reader.readLine(line)) {
    if (line.empty()) {
      continue;
    }
    if (text::startsWithIgnoreCase(line, "Format:")) {
      const DialogueFormat parsed = parseDialogueFormat(line);
      if (parsed.valid) {
        format = parsed;
      }
      continue;
    }
    if (line.rfind("Dialogue:", 0) != 0) {
      continue;
    }
    if (!format.valid) {
      continue;
    }
    const std::vector<std::string> values =
      text::split(std::string_view(line).substr(9), ',', format.length);
    if (values.size() != format.length) {
      continue;
    }
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!text::parseSsaTimestamp(values[format.start], startUs) ||
        !text::parseSsaTimestamp(values[format.end], endUs) || endUs <= startUs) {
      continue;
    }
    int layer = 0;
    if (format.layer != kIndexUnset) {
      int64_t parsedLayer = 0;
      if (text::parseInteger(text::trim(values[format.layer]), parsedLayer)) {
        layer = static_cast<int>(parsedLayer);
      }
    }
    const Style* style = nullptr;
    if (format.style != kIndexUnset && impl.haveStyles) {
      const auto found = impl.styles.find(text::trim(values[format.style]));
      if (found != impl.styles.end()) {
        style = &found->second;
      }
    }
    ParsedDialogue dialogue =
      buildDialogue(values[format.text], layer, style, impl.screenWidth, impl.screenHeight);
    dialogue.startUs = startUs;
    dialogue.endUs = endUs;
    dialogues.push_back(std::move(dialogue));
  }

  // A dialogue line may span several cues that start or end at different times, so the cues are
  // grouped by the boundaries of the document: a cue is shown from the boundary it starts at until
  // the next one, and the group of each interval holds every cue that covers it.
  std::vector<int64_t> boundaries;
  std::vector<std::vector<AVCue>> cueGroups;
  const auto boundaryIndex = [&](const int64_t timeUs) -> size_t {
    for (size_t i = boundaries.size(); i > 0; --i) {
      if (boundaries[i - 1] == timeUs) {
        return i - 1;
      }
      if (boundaries[i - 1] < timeUs) {
        boundaries.insert(boundaries.begin() + static_cast<std::ptrdiff_t>(i), timeUs);
        cueGroups.insert(cueGroups.begin() + static_cast<std::ptrdiff_t>(i),
                         i == 0 ? std::vector<AVCue>() : cueGroups[i - 1]);
        return i;
      }
    }
    boundaries.insert(boundaries.begin(), timeUs);
    cueGroups.insert(cueGroups.begin(), std::vector<AVCue>());
    return 0;
  };
  for (const auto& dialogue : dialogues) {
    const size_t startIndex = boundaryIndex(dialogue.startUs);
    const size_t endIndex = boundaryIndex(dialogue.endUs);
    if (endIndex <= startIndex) {
      continue;
    }
    for (size_t i = startIndex; i < endIndex; ++i) {
      cueGroups[i].push_back(dialogue.cue);
    }
  }

  std::vector<AVCuesWithTiming> results;
  for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
    if (cueGroups[i].empty() && i != 0) {
      // An empty group states that nothing is shown, which the duration of the previous result
      // already says.
      continue;
    }
    AVCuesWithTiming result;
    result.cues = std::move(cueGroups[i]);
    result.startTimeUs = boundaries[i];
    result.durationUs = boundaries[i + 1] - boundaries[i];
    results.push_back(std::move(result));
  }

  text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues, output);
  return true;
}

void AVSsaParser::reset() {
  // The header of an initialized track is kept; one parsed from a document is replaced by the
  // header of the next document.
}

} // namespace coral
