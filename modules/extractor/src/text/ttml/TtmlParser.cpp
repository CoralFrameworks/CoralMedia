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

#include <extractor/text/ttml/TtmlParser.h>

#include "text/SubtitleTextUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr int64_t kDefaultDurationUs = 2000000;

struct Style final {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikeout = false;
  bool hasColor = false;
  uint32_t color = 0;
  bool hasBackground = false;
  uint32_t background = 0;
  std::string fontFamily;
  std::string language;
  bool hasFontSize = false;
  float fontSize = 0.0f;
  bool fontSizeAbsolute = false;
  bool hasTextCombine = false;
  bool textCombine = false;
  bool hasEmphasis = false;
  AVCueSpan::Emphasis emphasis;
  bool hasShear = false;
  float shearPercentage = 0.0f;
  AVCue::Alignment multiRowAlignment = AVCue::Alignment::kUnset;

  [[nodiscard]] bool hasTextStyle() const {
    return bold || italic || underline || strikeout || !fontFamily.empty() || hasFontSize;
  }
};

struct Region final {
  Style style;
  bool hasOrigin = false;
  float x = 0.0f;
  float y = 0.0f;
  bool hasExtent = false;
  float width = 0.0f;
  float height = 0.0f;
  AVCue::Alignment displayAlign = AVCue::Alignment::kUnset;
  AVCue::VerticalType verticalType = AVCue::VerticalType::kUnset;
};

enum class RubyKind { kContainer, kBase, kText, kDelimiter };

struct RubyTag final {
  RubyKind kind = RubyKind::kContainer;
  size_t baseStart = 0;
  size_t baseEnd = 0;
  std::string annotation;
  AVCueSpan::RubyPosition position = AVCueSpan::RubyPosition::kUnknown;
};

struct Paragraph final {
  bool active = false;
  Style style;
  bool hasPosition = false;
  float position = 0.0f;
  float line = 0.0f;
  bool hasSize = false;
  float size = 0.0f;
  AVCue::VerticalType verticalType = AVCue::VerticalType::kUnset;
  int64_t beginUs = 0;
  int64_t endUs = kDefaultDurationUs;
  bool hasEnd = false;
  int64_t durationUs = Definitions::kTimeUnset;
  AVCue::Alignment alignment = AVCue::Alignment::kUnset;
  std::string text;
  std::vector<AVCueSpan> spans;
};

std::string tagName(std::string_view name) {
  const size_t colon = name.rfind(':');
  std::string result = colon == std::string_view::npos ? std::string(name)
                                                        : std::string(name.substr(colon + 1));
  for (char& character : result) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return result;
}

bool isSpace(const char character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

std::map<std::string, std::string> parseAttributes(std::string_view tag) {
  std::map<std::string, std::string> attributes;
  size_t position = 0;
  while (position < tag.size() && !isSpace(tag[position]) && tag[position] != '/') ++position;
  while (position < tag.size()) {
    while (position < tag.size() && isSpace(tag[position])) ++position;
    const size_t nameStart = position;
    while (position < tag.size() && !isSpace(tag[position]) && tag[position] != '='
           && tag[position] != '/') {
      ++position;
    }
    if (position == nameStart) break;
    std::string name(tag.substr(nameStart, position - nameStart));
    while (position < tag.size() && isSpace(tag[position])) ++position;
    if (position >= tag.size() || tag[position] != '=') {
      attributes.emplace(std::move(name), std::string());
      continue;
    }
    ++position;
    while (position < tag.size() && isSpace(tag[position])) ++position;
    if (position >= tag.size() || (tag[position] != '"' && tag[position] != '\'')) continue;
    const char quote = tag[position++];
    const size_t valueStart = position;
    while (position < tag.size() && tag[position] != quote) ++position;
    attributes.emplace(std::move(name), std::string(tag.substr(valueStart, position - valueStart)));
    if (position < tag.size()) ++position;
  }
  return attributes;
}

std::string decodeEntities(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '&') {
      result.push_back(value[i]);
      continue;
    }
    const size_t end = value.find(';', i + 1);
    if (end == std::string_view::npos || end - i > 12) {
      result.push_back('&');
      continue;
    }
    const std::string entity(value.substr(i + 1, end - i - 1));
    if (entity == "amp") result.push_back('&');
    else if (entity == "lt") result.push_back('<');
    else if (entity == "gt") result.push_back('>');
    else if (entity == "quot") result.push_back('"');
    else if (entity == "apos") result.push_back('\'');
    else if (entity == "nbsp") result += "\xC2\xA0";
    else if (!entity.empty() && entity[0] == '#') {
      char* numberEnd = nullptr;
      const long number = std::strtol(entity.c_str() + 1, &numberEnd, 10);
      if (numberEnd != nullptr && *numberEnd == '\0' && number > 0) {
        if (number < 0x80) {
          result.push_back(static_cast<char>(number));
        } else if (number < 0x800) {
          result.push_back(static_cast<char>(0xC0 | (number >> 6)));
          result.push_back(static_cast<char>(0x80 | (number & 0x3F)));
        } else {
          result.push_back(static_cast<char>(0xE0 | (number >> 12)));
          result.push_back(static_cast<char>(0x80 | ((number >> 6) & 0x3F)));
          result.push_back(static_cast<char>(0x80 | (number & 0x3F)));
        }
      } else {
        result.push_back('&');
      }
    } else {
      result.push_back('&');
    }
    i = end;
  }
  return result;
}

bool parseTime(std::string_view value, int64_t& timeUs) {
  std::string text = coral::text::trim(value);
  if (text.empty()) return false;
  for (char& character : text) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  if (coral::text::endsWith(text, "ms")) {
    float milliseconds = 0.0f;
    if (!coral::text::parseFloat(text.substr(0, text.size() - 2), milliseconds)) return false;
    timeUs = static_cast<int64_t>(milliseconds * 1000.0f);
    return true;
  }
  if (coral::text::endsWith(text, "s")) {
    float seconds = 0.0f;
    if (!coral::text::parseFloat(text.substr(0, text.size() - 1), seconds)) return false;
    timeUs = static_cast<int64_t>(seconds * 1000000.0f);
    return true;
  }
  if (text.find(':') == std::string::npos) {
    float seconds = 0.0f;
    if (!coral::text::parseFloat(text, seconds)) return false;
    timeUs = static_cast<int64_t>(seconds * 1000000.0f);
    return true;
  }
  const std::vector<std::string> fields = coral::text::split(text, ':');
  if (fields.size() < 2 || fields.size() > 3) return false;
  int64_t hours = 0;
  int64_t minutes = 0;
  if (fields.size() == 3) {
    if (!coral::text::parseInteger(fields[0], hours) ||
        !coral::text::parseInteger(fields[1], minutes)) {
      return false;
    }
  } else if (!coral::text::parseInteger(fields[0], minutes)) {
    return false;
  }
  float seconds = 0.0f;
  if (!coral::text::parseFloat(fields.back(), seconds)) return false;
  timeUs = ((hours * 3600 + minutes * 60) * 1000000) + static_cast<int64_t>(seconds * 1000000.0f);
  return true;
}

/** The resolved timing of one TTML element, in the timeline of the document. */
struct Timing final {
  int64_t startUs = 0;
  int64_t endUs = Definitions::kTimeUnset;
  bool hasStart = false;
  bool hasEnd = false;
};

bool attributeTime(const std::map<std::string, std::string>& attributes, const std::string& name,
  int64_t& timeUs) {
  const auto found = attributes.find(name);
  return found != attributes.end() && parseTime(found->second, timeUs);
}

/**
 * Resolves `begin`, `end` and `dur` against the timing of the parent element (SMIL timing).
 *
 * <p>A `begin` is relative to the parent's start; without one the element starts with the parent.
 * Without an `end`, `dur` supplies it, and otherwise the parent's end is inherited, which is the
 * rule the reference implementation applies to a `<p>` wrapped in a timed `<div>`.
 */
Timing resolveTiming(const std::map<std::string, std::string>& attributes, const Timing& parent) {
  int64_t beginUs = 0;
  int64_t endUs = 0;
  int64_t durationUs = 0;
  const bool hasBegin = attributeTime(attributes, "begin", beginUs);
  const bool hasEnd = attributeTime(attributes, "end", endUs);
  const bool hasDuration = attributeTime(attributes, "dur", durationUs);
  Timing timing;
  if (parent.hasStart) {
    timing.startUs = hasBegin ? parent.startUs + beginUs : parent.startUs;
    timing.hasStart = true;
    if (hasEnd) {
      timing.endUs = parent.startUs + endUs;
      timing.hasEnd = true;
    }
  } else {
    if (hasBegin) {
      timing.startUs = beginUs;
      timing.hasStart = true;
    }
    if (hasEnd) {
      timing.endUs = endUs;
      timing.hasEnd = true;
    }
  }
  if (!timing.hasEnd) {
    if (hasDuration && timing.hasStart) {
      timing.endUs = timing.startUs + durationUs;
      timing.hasEnd = true;
    } else if (parent.hasEnd) {
      timing.endUs = parent.endUs;
      timing.hasEnd = true;
    }
  }
  return timing;
}

/** Decodes the base64 payload of a TTML `<image>` element, ignoring whitespace. */
std::vector<uint8_t> decodeBase64(const std::string_view encoded) {
  std::vector<uint8_t> decoded;
  decoded.reserve(encoded.size() * 3 / 4);
  int value = 0;
  int bits = -8;
  for (const char character: encoded) {
    int digit = -1;
    if (character >= 'A' && character <= 'Z') digit = character - 'A';
    else if (character >= 'a' && character <= 'z') digit = character - 'a' + 26;
    else if (character >= '0' && character <= '9') digit = character - '0' + 52;
    else if (character == '+') digit = 62;
    else if (character == '/') digit = 63;
    if (digit < 0) continue;
    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 0) {
      decoded.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return decoded;
}

bool parseColor(std::string_view value, uint32_t& color) {
  std::string text = coral::text::trim(value);
  if (text.empty()) return false;
  if (text[0] == '#') {
    const std::string digits = text.substr(1);
    unsigned long parsed = 0;
    for (const char character : digits) {
      int digit = -1;
      if (character >= '0' && character <= '9') digit = character - '0';
      else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
      else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
      else return false;
      parsed = parsed * 16 + static_cast<unsigned long>(digit);
    }
    if (digits.size() == 6) {
      color = 0xFF000000u | static_cast<uint32_t>(parsed);
      return true;
    }
    if (digits.size() == 8) {
      color = static_cast<uint32_t>(parsed);
      return true;
    }
    return false;
  }
  if (text == "black") color = 0xFF000000u;
  else if (text == "white") color = 0xFFFFFFFFu;
  else if (text == "red") color = 0xFFFF0000u;
  else if (text == "green") color = 0xFF00FF00u;
  else if (text == "blue") color = 0xFF0000FFu;
  else if (text == "yellow") color = 0xFFFFFF00u;
  else if (text == "cyan") color = 0xFF00FFFFu;
  else if (text == "magenta") color = 0xFFFF00FFu;
  else return false;
  return true;
}

AVCue::Alignment parseAlignment(std::string_view value);

/** Parses a TTML `tts:fontSize` value: `px` is absolute, `em` and `%` are relative to the cue. */
bool parseTtmlFontSize(const std::string_view expression, Style& style) {
  std::string value = coral::text::trim(expression);
  // TTML allows a vertical and a horizontal value; the reference implementation keeps the second.
  const size_t space = value.find_last_of(" \t\r\n");
  if (space != std::string::npos) {
    value = coral::text::trim(value.substr(space + 1));
  }
  if (value.size() < 2) {
    return false;
  }
  bool absolute = false;
  bool percent = false;
  size_t unitStart = value.size();
  if (value.compare(value.size() - 2, 2, "px") == 0) {
    absolute = true;
    unitStart = value.size() - 2;
  } else if (value.compare(value.size() - 2, 2, "em") == 0) {
    unitStart = value.size() - 2;
  } else if (value.back() == '%') {
    percent = true;
    unitStart = value.size() - 1;
  } else {
    return false;
  }
  float size = 0.0f;
  if (!coral::text::parseFloat(coral::text::trim(value.substr(0, unitStart)), size) || size <= 0.0f) {
    return false;
  }
  style.hasFontSize = true;
  style.fontSize = percent ? size / 100.0f : size;
  style.fontSizeAbsolute = absolute;
  return true;
}

/** Parses a TTML `tts:shear` value, a signed percentage clamped to [-100, 100]. */
bool parseTtmlShear(const std::string_view expression, Style& style) {
  std::string value = coral::text::trim(expression);
  if (value.empty() || value.back() != '%') {
    return false;
  }
  value.pop_back();
  float percentage = 0.0f;
  if (!coral::text::parseFloat(coral::text::trim(value), percentage)) {
    return false;
  }
  style.hasShear = true;
  style.shearPercentage = std::clamp(percentage, -100.0f, 100.0f);
  return true;
}

/**
 * Parses a TTML `tts:textEmphasis` value, e.g. `filled circle before` or `dot after`.
 *
 * <p>The fill (`filled`/`open`) is ignored because the span model only stores the shape, the
 * position and a colour; `outside` is treated as `before`, which is what the reference renderer
 * does after resolving it.
 */
void parseTextEmphasis(const std::string_view expression, Style& style) {
  const std::string value = coral::text::toLowerAscii(coral::text::trim(expression));
  if (value.empty()) {
    return;
  }
  bool none = false;
  bool dot = false;
  bool sesame = false;
  bool before = false;
  bool after = false;
  for (const auto& word: coral::text::split(value, ' ')) {
    if (word.empty()) continue;
    if (word == "none") none = true;
    else if (word == "dot") dot = true;
    else if (word == "sesame") sesame = true;
    else if (word == "before") before = true;
    else if (word == "after") after = true;
  }
  AVCueSpan::Emphasis emphasis;
  if (none) {
    emphasis.mark = AVCueSpan::TextEmphasisMark::kNone;
  } else if (dot) {
    emphasis.mark = AVCueSpan::TextEmphasisMark::kDot;
  } else if (sesame) {
    emphasis.mark = AVCueSpan::TextEmphasisMark::kSesame;
  } else {
    // `auto` and a missing shape resolve to a filled circle in horizontal text.
    emphasis.mark = AVCueSpan::TextEmphasisMark::kCircle;
  }
  emphasis.mode = after ? AVCueSpan::TextEmphasisMode::kAfter
                        : AVCueSpan::TextEmphasisMode::kBefore;
  style.hasEmphasis = true;
  style.emphasis = emphasis;
}

/** Applies one TTML style declaration to @p style; unknown names are ignored. */
void applyStyleDeclaration(const std::string& name, const std::string& value, Style& style) {
  if (name == "color" || name == "tts:color") {
    style.hasColor = parseColor(value, style.color);
  } else if (name == "background-color" || name == "tts:backgroundcolor") {
    style.hasBackground = parseColor(value, style.background);
  } else if (name == "font-style" || name == "tts:fontstyle") {
    style.italic = coral::text::toLowerAscii(value) == "italic";
  } else if (name == "font-weight" || name == "tts:fontweight") {
    style.bold = coral::text::toLowerAscii(value) == "bold";
  } else if (name == "text-decoration" || name == "tts:textdecoration") {
    const std::string decoration = coral::text::toLowerAscii(value);
    style.underline = decoration.find("underline") != std::string::npos;
    style.strikeout = decoration.find("linethrough") != std::string::npos;
  } else if (name == "font-family" || name == "tts:fontfamily") {
    style.fontFamily = value;
  } else if (name == "font-size" || name == "tts:fontsize") {
    parseTtmlFontSize(value, style);
  } else if (name == "text-combine" || name == "tts:textcombine") {
    const std::string lowered = coral::text::toLowerAscii(coral::text::trim(value));
    if (lowered == "all") {
      style.hasTextCombine = true;
      style.textCombine = true;
    } else if (lowered == "none") {
      style.hasTextCombine = true;
      style.textCombine = false;
    }
  } else if (name == "text-emphasis" || name == "tts:textemphasis") {
    parseTextEmphasis(value, style);
  } else if (name == "shear" || name == "tts:shear") {
    parseTtmlShear(value, style);
  } else if (name == "multirowalign" || name == "tts:multirowalign" ||
             name == "ebutts:multirowalign") {
    const AVCue::Alignment alignment = parseAlignment(value);
    if (alignment != AVCue::Alignment::kUnset) {
      style.multiRowAlignment = alignment;
    }
  }
}

Style parseStyle(std::string_view style, const Style& parent) {
  Style result = parent;
  const std::vector<std::string> declarations = coral::text::split(style, ';');
  for (const auto& declaration : declarations) {
    const size_t colon = declaration.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = coral::text::toLowerAscii(coral::text::trim(declaration.substr(0, colon)));
    const std::string value = coral::text::trim(declaration.substr(colon + 1));
    applyStyleDeclaration(key, value, result);
  }
  return result;
}

Style applyStyleAttributes(const std::map<std::string, std::string>& attributes, const Style& base) {
  Style result = base;
  for (const auto& [rawName, value]: attributes) {
    applyStyleDeclaration(coral::text::toLowerAscii(rawName), value, result);
  }
  return result;
}
bool parsePercentage(std::string_view value, float& fraction) {
  const std::string text = coral::text::trim(value);
  if (text.empty() || text.back() != '%') return false;
  float percentage = 0.0f;
  if (!coral::text::parseFloat(text.substr(0, text.size() - 1), percentage)) return false;
  fraction = percentage / 100.0f;
  return true;
}

AVCue::Alignment parseAlignment(std::string_view value) {
  const std::string alignment = coral::text::toLowerAscii(coral::text::trim(value));
  if (alignment == "left" || alignment == "start") return AVCue::Alignment::kLeft;
  if (alignment == "center") return AVCue::Alignment::kCenter;
  if (alignment == "right" || alignment == "end") return AVCue::Alignment::kRight;
  return AVCue::Alignment::kUnset;
}

AVCueSpan makeStyleSpan(const Style& style, size_t start, size_t end) {
  AVCueSpan span;
  span.type = AVCueSpan::Type::kTextStyle;
  span.start = start;
  span.end = end;
  span.style.bold = style.bold;
  span.style.italic = style.italic;
  span.style.underline = style.underline;
  span.style.strikeout = style.strikeout;
  span.style.fontFamily = style.fontFamily;
  if (style.hasFontSize) {
    span.style.textSize = style.fontSize;
    span.style.textSizeAbsolute = style.fontSizeAbsolute;
  }
  return span;
}

} // namespace

class AVTtmlParser::Impl final {
public:
  void feed(const uint8_t* data, size_t size, std::vector<AVCuesWithTiming>& results) {
    const std::string document(reinterpret_cast<const char*>(data), size);
    Paragraph paragraph;
    std::vector<Style> styleStack;
    styleStack.push_back(Style{});
    std::vector<std::string> regionStack;
    std::vector<RubyTag> rubyTags;
    std::string rubyText;
    std::string currentRegion;
    bool inStyleHeader = false;
    std::map<std::string, std::vector<uint8_t>> imageMap;
    std::vector<Timing> timingStack;
    Timing rootTiming;
    rootTiming.hasStart = true;
    timingStack.push_back(rootTiming);

    size_t position = 0;
    while (position < document.size()) {
      if (document.compare(position, 4, "<!--") == 0) {
        const size_t end = document.find("-->", position + 4);
        position = end == std::string::npos ? document.size() : end + 3;
        continue;
      }
      if (document[position] != '<') {
        const size_t next = document.find('<', position);
        const size_t end = next == std::string::npos ? document.size() : next;
        if (paragraph.active) {
          appendText(paragraph, styleStack.back(), decodeEntities(
              std::string_view(document).substr(position, end - position)), rubyTags, rubyText);
        }
        position = end;
        continue;
      }
      const size_t tagEnd = document.find('>', position + 1);
      if (tagEnd == std::string::npos) break;
      const std::string_view tag(document.data() + position + 1, tagEnd - position - 1);
      position = tagEnd + 1;
      if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
      const bool closing = tag[0] == '/';
      const bool selfClosing = !tag.empty() && tag.back() == '/';
      std::string_view name = closing ? tag.substr(1) : tag;
      while (!name.empty() && (name.front() == '/' || isSpace(name.front()))) name.remove_prefix(1);
      const size_t nameEnd = name.find_first_of(" \t\r\n/");
      const std::string local = tagName(name.substr(0, nameEnd));
      const auto attributes = parseAttributes(tag);

      if (closing) {
        if (local == "p" && paragraph.active) {
          finishParagraph(paragraph, results);
          paragraph = Paragraph{};
        } else if (local == "span" && styleStack.size() > 1) {
          if (!rubyTags.empty()) {
            RubyTag tag = std::move(rubyTags.back());
            rubyTags.pop_back();
            if (tag.kind == RubyKind::kText) {
              for (auto it = rubyTags.rbegin(); it != rubyTags.rend(); ++it) {
                if (it->kind == RubyKind::kContainer) {
                  it->annotation = rubyText;
                  break;
                }
              }
              rubyText.clear();
            } else if (tag.kind == RubyKind::kBase) {
              for (auto it = rubyTags.rbegin(); it != rubyTags.rend(); ++it) {
                if (it->kind == RubyKind::kContainer) {
                  it->baseEnd = paragraph.text.size();
                  break;
                }
              }
            } else if (tag.kind == RubyKind::kContainer) {
              for (auto it = rubyTags.rbegin(); it != rubyTags.rend(); ++it) {
                if (it->kind == RubyKind::kContainer) {
                  tag = std::move(*it);
                  rubyTags.erase(std::next(it).base());
                  break;
                }
              }
              if (tag.baseEnd > tag.baseStart && !tag.annotation.empty()) {
                AVCueSpan span;
                span.type = AVCueSpan::Type::kRuby;
                span.start = tag.baseStart;
                span.end = tag.baseEnd;
                span.ruby.text = tag.annotation;
                span.ruby.type = AVCueSpan::RubyType::kBase;
                span.ruby.position = tag.position;
                paragraph.spans.push_back(std::move(span));
              }
            }
          }
          styleStack.pop_back();
        } else if (local == "style") {
          inStyleHeader = false;
        } else if (local == "div" || local == "body") {
          if (local == "div" && !regionStack.empty()) {
            currentRegion = regionStack.back();
            regionStack.pop_back();
          }
          if (timingStack.size() > 1) {
            timingStack.pop_back();
          }
        }
        continue;
      }

      if (local == "image") {
        std::string id;
        if (const auto found = attributes.find("xml:id"); found != attributes.end()) id = found->second;
        else if (const auto found2 = attributes.find("id"); found2 != attributes.end()) id = found2->second;
        const size_t imageEnd = document.find("</image>", position);
        if (!id.empty() && imageEnd != std::string::npos) {
          imageMap[id] = decodeBase64(
            std::string_view(document).substr(position, imageEnd - position));
          position = imageEnd + 8;
        }
        continue;
      }
      if (local == "region") {
        parseRegion(attributes);
        continue;
      }
      if (local == "body") {
        const Timing bodyTiming = resolveTiming(attributes, timingStack.back());
        if (!selfClosing) {
          timingStack.push_back(bodyTiming);
        }
        continue;
      }
      if (local == "div") {
        const Timing divTiming = resolveTiming(attributes, timingStack.back());
        regionStack.push_back(currentRegion);
        if (const auto region = attributes.find("region"); region != attributes.end()) {
          currentRegion = coral::text::trim(region->second);
        }
        std::string imageId;
        for (const auto& [rawName, value]: attributes) {
          const std::string name = coral::text::toLowerAscii(rawName);
          if ((name == "backgroundimage" || name == "tts:backgroundimage") && !value.empty() &&
              value.front() == '#') {
            imageId = value.substr(1);
          }
        }
        if (!imageId.empty()) {
          emitImageCue(imageId, divTiming, currentRegion, regions, imageMap, results);
        }
        if (!selfClosing) {
          timingStack.push_back(divTiming);
        }
        continue;
      }
      if (local == "style") {
        inStyleHeader = true;
        std::string id;
        if (const auto found = attributes.find("xml:id"); found != attributes.end()) id = found->second;
        else if (const auto found2 = attributes.find("id"); found2 != attributes.end()) id = found2->second;
        if (!id.empty()) {
          Style style = applyStyleAttributes(attributes, Style{});
          if (const auto based = attributes.find("style"); based != attributes.end()) {
            const auto found = styleSheet.find(coral::text::trim(based->second));
            if (found != styleSheet.end()) style = mergeStyles(style, found->second);
          }
          styleSheet[id] = std::move(style);
        }
        inStyleHeader = !selfClosing;
        continue;
      }
      if (inStyleHeader) continue;
      if (local == "p") {
        if (paragraph.active) finishParagraph(paragraph, results);
        paragraph = Paragraph{};
        paragraph.active = true;
        const Style base = styleStack.empty() ? Style{} : styleStack.front();
        Style paragraphStyle = applyStyleAttributes(attributes, base);
        if (const auto styleId = attributes.find("style"); styleId != attributes.end()) {
          const auto found = styleSheet.find(coral::text::trim(styleId->second));
          if (found != styleSheet.end()) paragraphStyle = mergeStyles(paragraphStyle, found->second);
        }
        paragraph.style = paragraphStyle;
        styleStack.clear();
        styleStack.push_back(paragraph.style);
        paragraph.hasPosition = false;
        paragraph.hasSize = false;
        paragraph.verticalType = AVCue::VerticalType::kUnset;
        Style paragraphStyle2 = paragraphStyle;
        std::string regionId;
        if (const auto found = attributes.find("region"); found != attributes.end()) {
          regionId = coral::text::trim(found->second);
        } else {
          regionId = currentRegion;
        }
        if (!regionId.empty()) {
          const auto found = regions.find(regionId);
          if (found != regions.end()) {
            paragraphStyle2 = mergeStyles(paragraphStyle2, found->second.style);
            if (found->second.hasOrigin) {
              paragraph.hasPosition = true;
              paragraph.position = found->second.x;
              paragraph.line = found->second.y;
            }
            if (found->second.hasExtent) {
              paragraph.hasSize = true;
              paragraph.size = found->second.width;
            }
            paragraph.verticalType = found->second.verticalType;
          }
        }
        paragraph.style = paragraphStyle2;
        styleStack.clear();
        styleStack.push_back(paragraph.style);
        parseParagraphAttributes(attributes, paragraph, timingStack.back());
      } else if (local == "br") {
        if (paragraph.active) paragraph.text.push_back('\x01');
      } else if (local == "span") {
        Style style = applyStyleAttributes(attributes, styleStack.back());
        RubyTag rubyTag;
        bool isRuby = false;
        if (const auto found = attributes.find("tts:ruby"); found != attributes.end()) {
          const std::string kind = coral::text::toLowerAscii(coral::text::trim(found->second));
          isRuby = true;
          if (kind == "container") rubyTag.kind = RubyKind::kContainer;
          else if (kind == "base") rubyTag.kind = RubyKind::kBase;
          else if (kind == "text") rubyTag.kind = RubyKind::kText;
          else rubyTag.kind = RubyKind::kDelimiter;
        }
        if (isRuby) {
          if (rubyTag.kind == RubyKind::kContainer) {
            if (const auto pos = attributes.find("tts:rubyPosition"); pos != attributes.end()) {
              const std::string value = coral::text::toLowerAscii(coral::text::trim(pos->second));
              if (value == "before") rubyTag.position = AVCueSpan::RubyPosition::kBefore;
              else if (value == "after") rubyTag.position = AVCueSpan::RubyPosition::kAfter;
            }
          } else if (rubyTag.kind == RubyKind::kBase) {
            rubyTag.baseStart = paragraph.text.size();
            for (auto it = rubyTags.rbegin(); it != rubyTags.rend(); ++it) {
              if (it->kind == RubyKind::kContainer) {
                it->baseStart = paragraph.text.size();
                break;
              }
            }
          } else if (rubyTag.kind == RubyKind::kText) {
            rubyText.clear();
          }
          rubyTags.push_back(std::move(rubyTag));
        }
        if (const auto styleId = attributes.find("style"); styleId != attributes.end()) {
          const auto found = styleSheet.find(coral::text::trim(styleId->second));
          if (found != styleSheet.end()) style = mergeStyles(style, found->second);
        }
        styleStack.push_back(std::move(style));
      }
    }
    if (paragraph.active) finishParagraph(paragraph, results);
  }

  void reset() {
    styleSheet.clear();
  }

private:
  void parseRegion(const std::map<std::string, std::string>& attributes) {
    std::string id;
    if (const auto found = attributes.find("xml:id"); found != attributes.end()) id = found->second;
    else if (const auto found2 = attributes.find("id"); found2 != attributes.end()) id = found2->second;
    if (id.empty()) return;
    Region region;
    region.style = applyStyleAttributes(attributes, Style{});
    for (const auto& [rawName, value]: attributes) {
      const std::string name = coral::text::toLowerAscii(rawName);
      if (name == "tts:origin" || name == "ttm:origin" || name == "origin") {
        const auto fields = coral::text::split(value, ' ');
        if (fields.size() == 2 && parsePercentage(fields[0], region.x) &&
            parsePercentage(fields[1], region.y)) {
          region.hasOrigin = true;
        }
      } else if (name == "tts:extent" || name == "ttm:extent" || name == "extent") {
        const auto fields = coral::text::split(value, ' ');
        if (fields.size() == 2 && parsePercentage(fields[0], region.width) &&
            parsePercentage(fields[1], region.height)) {
          region.hasExtent = true;
        }
      } else if (name == "tts:displayalign" || name == "displayalign") {
        region.displayAlign = parseAlignment(value);
      } else if (name == "tts:writingmode" || name == "writingmode") {
        const std::string mode = coral::text::toLowerAscii(coral::text::trim(value));
        if (mode == "tbrl") region.verticalType = AVCue::VerticalType::kRightToLeft;
        else if (mode == "tblr") region.verticalType = AVCue::VerticalType::kLeftToRight;
      }
    }
    regions[id] = std::move(region);
  }

  static std::string attribute(const std::map<std::string, std::string>& attributes,
      const std::string& name) {
    const auto found = attributes.find(name);
    return found == attributes.end() ? std::string() : found->second;
  }

  static Style mergeStyles(const Style& first, const Style& second) {
    Style result = first;
    if (second.bold) result.bold = true;
    if (second.italic) result.italic = true;
    if (second.underline) result.underline = true;
    if (second.strikeout) result.strikeout = true;
    if (second.hasColor) { result.hasColor = true; result.color = second.color; }
    if (second.hasBackground) { result.hasBackground = true; result.background = second.background; }
    if (!second.fontFamily.empty()) result.fontFamily = second.fontFamily;
    if (!second.language.empty()) result.language = second.language;
    if (second.hasFontSize) {
      result.hasFontSize = true;
      result.fontSize = second.fontSize;
      result.fontSizeAbsolute = second.fontSizeAbsolute;
    }
    if (second.hasTextCombine) {
      result.hasTextCombine = true;
      result.textCombine = second.textCombine;
    }
    if (second.hasEmphasis) {
      result.hasEmphasis = true;
      result.emphasis = second.emphasis;
    }
    if (second.hasShear) {
      result.hasShear = true;
      result.shearPercentage = second.shearPercentage;
    }
    if (second.multiRowAlignment != AVCue::Alignment::kUnset) {
      result.multiRowAlignment = second.multiRowAlignment;
    }
    return result;
  }

  static void parseParagraphAttributes(const std::map<std::string, std::string>& attributes,
      Paragraph& paragraph, const Timing& parent) {
    const Timing timing = resolveTiming(attributes, parent);
    paragraph.beginUs = timing.startUs;
    paragraph.hasEnd = timing.hasEnd;
    if (timing.hasEnd) {
      paragraph.endUs = timing.endUs;
      paragraph.durationUs = timing.endUs - timing.startUs;
    } else {
      paragraph.durationUs = kDefaultDurationUs;
    }
    if (paragraph.durationUs <= 0) paragraph.durationUs = kDefaultDurationUs;
    for (const auto& [rawName, value]: attributes) {
      const std::string name = coral::text::toLowerAscii(rawName);
      if (name == "tts:origin" || name == "ttm:origin" || name == "origin") {
        const auto fields = coral::text::split(value, ' ');
        if (fields.size() == 2 && parsePercentage(fields[0], paragraph.position) &&
            parsePercentage(fields[1], paragraph.line)) {
          paragraph.hasPosition = true;
        }
      } else if (name == "tts:extent" || name == "ttm:extent" || name == "extent") {
        float width = 0.0f;
        float height = 0.0f;
        const auto fields = coral::text::split(value, ' ');
        if (fields.size() == 2 && parsePercentage(fields[0], width) &&
            parsePercentage(fields[1], height)) {
          paragraph.hasSize = true;
          paragraph.size = width;
        }
      } else if (name == "tts:writingmode" || name == "writingmode") {
        const std::string mode = coral::text::toLowerAscii(coral::text::trim(value));
        if (mode == "tbrl") paragraph.verticalType = AVCue::VerticalType::kRightToLeft;
        else if (mode == "tblr") paragraph.verticalType = AVCue::VerticalType::kLeftToRight;
      }
    }
    if (const auto align = attributes.find("tts:textAlign"); align != attributes.end()) {
      paragraph.alignment = parseAlignment(align->second);
    } else if (const auto align2 = attributes.find("textAlign"); align2 != attributes.end()) {
      paragraph.alignment = parseAlignment(align2->second);
    }
  }

  static void emitImageCue(const std::string& imageId, const Timing& timing,
      const std::string& regionId, const std::map<std::string, Region>& regions,
      const std::map<std::string, std::vector<uint8_t>>& imageMap,
      std::vector<AVCuesWithTiming>& results) {
    const auto image = imageMap.find(imageId);
    if (image == imageMap.end() || image->second.empty()) return;
    AVCue cue;
    cue.bitmap.encoded = image->second;
    cue.positionAnchor = AVCue::AnchorType::kStart;
    cue.lineType = AVCue::LineType::kFraction;
    if (const auto region = regions.find(regionId); region != regions.end()) {
      if (region->second.hasOrigin) {
        cue.position = region->second.x;
        cue.line = region->second.y;
      }
      if (region->second.hasExtent) {
        cue.size = region->second.width;
        cue.bitmapHeight = region->second.height;
      }
      cue.verticalType = region->second.verticalType;
      switch (region->second.displayAlign) {
        case AVCue::Alignment::kCenter:
          cue.lineAnchor = AVCue::AnchorType::kMiddle;
          break;
        case AVCue::Alignment::kRight:
          cue.lineAnchor = AVCue::AnchorType::kEnd;
          break;
        default:
          cue.lineAnchor = AVCue::AnchorType::kStart;
          break;
      }
    } else {
      cue.lineAnchor = AVCue::AnchorType::kStart;
    }
    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = timing.hasStart ? timing.startUs : 0;
    result.durationUs = timing.hasEnd && timing.endUs > result.startTimeUs
                          ? timing.endUs - result.startTimeUs
                          : kDefaultDurationUs;
    results.push_back(std::move(result));
  }

  static void finishParagraph(Paragraph& paragraph, std::vector<AVCuesWithTiming>& results) {
    if (paragraph.text.empty()) return;
    std::string normalized;
    std::vector<size_t> offsets(paragraph.text.size() + 1, 0);
    bool pendingSpace = false;
    for (size_t i = 0; i < paragraph.text.size(); ++i) {
      offsets[i] = normalized.size();
      if (pendingSpace && !normalized.empty() && normalized.back() != '\n') ++offsets[i];
      const char character = paragraph.text[i];
      if (character == '\x01') {
        if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
        normalized.push_back('\n');
        pendingSpace = false;
      } else if (character == ' ' || character == '\t' || character == '\r' ||
                 character == '\n') {
        pendingSpace = true;
      } else {
        if (pendingSpace && !normalized.empty() && normalized.back() != '\n') {
          normalized.push_back(' ');
        }
        pendingSpace = false;
        normalized.push_back(character);
      }
    }
    offsets[paragraph.text.size()] = normalized.size();
    while (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '\n')) {
      normalized.pop_back();
    }
    for (auto& span: paragraph.spans) {
      span.start = offsets[std::min(span.start, paragraph.text.size())];
      span.end = offsets[std::min(span.end, paragraph.text.size())];
    }
    AVCue cue;
    cue.text = std::move(normalized);
    cue.spans = std::move(paragraph.spans);
    cue.textAlignment = paragraph.alignment;
    if (paragraph.hasPosition) {
      cue.position = paragraph.position;
      cue.line = paragraph.line;
      cue.positionAnchor = AVCue::AnchorType::kStart;
      cue.lineAnchor = AVCue::AnchorType::kStart;
    }
    if (paragraph.hasSize) cue.size = paragraph.size;
    cue.verticalType = paragraph.verticalType;
    cue.multiRowAlignment = paragraph.style.multiRowAlignment;
    if (paragraph.style.hasShear) {
      // TTML states the shear as a percentage of 90 degrees, positive to the right; the cue field
      // is in graphics coordinates, so the sign flips, as in the reference implementation.
      cue.shearDegrees = paragraph.style.shearPercentage * -90.0f / 100.0f;
    }
    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = paragraph.beginUs;
    result.durationUs = paragraph.durationUs;
    result.cueTimesUs.push_back(0);
    result.cueDurationsUs.push_back(paragraph.durationUs);
    results.push_back(std::move(result));
    paragraph.active = false;
  }

  static void appendText(Paragraph& paragraph, const Style& style, const std::string& text,
      std::vector<RubyTag>& rubyTags, std::string& rubyText) {
    if (text.empty()) return;
    if (!rubyTags.empty() && (rubyTags.back().kind == RubyKind::kText ||
                              rubyTags.back().kind == RubyKind::kDelimiter)) {
      rubyText.append(text);
      return;
    }
    if (!rubyTags.empty()) {
      for (auto it = rubyTags.rbegin(); it != rubyTags.rend(); ++it) {
        if (it->kind == RubyKind::kContainer) break;
        if (it->kind == RubyKind::kText || it->kind == RubyKind::kDelimiter) {
          rubyText.append(text);
          return;
        }
      }
    }
    const size_t start = paragraph.text.size();
    paragraph.text.append(text);
    const size_t end = paragraph.text.size();
    if (style.language.empty() && !style.hasTextStyle() && !style.hasColor && !style.hasBackground &&
        !style.hasTextCombine && !style.hasEmphasis) {
      return;
    }
    if (style.hasTextStyle()) paragraph.spans.push_back(makeStyleSpan(style, start, end));
    if (style.hasTextCombine && style.textCombine) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kHorizontalTextInVerticalContext;
      span.start = start;
      span.end = end;
      paragraph.spans.push_back(std::move(span));
    }
    if (style.hasEmphasis) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kTextEmphasis;
      span.start = start;
      span.end = end;
      span.emphasis = style.emphasis;
      paragraph.spans.push_back(std::move(span));
    }
    if (style.hasColor) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kForegroundColor;
      span.start = start;
      span.end = end;
      span.color = style.color;
      paragraph.spans.push_back(std::move(span));
    }
    if (style.hasBackground) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kBackgroundColor;
      span.start = start;
      span.end = end;
      span.color = style.background;
      paragraph.spans.push_back(std::move(span));
    }
    if (!style.language.empty()) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kLanguage;
      span.start = start;
      span.end = end;
      span.text = style.language;
      paragraph.spans.push_back(std::move(span));
    }
  }

  std::map<std::string, Style> styleSheet;
  std::map<std::string, Region> regions;
};

AVTtmlParser::AVTtmlParser() : mImpl(new Impl()) {
}

AVTtmlParser::~AVTtmlParser() = default;

bool AVTtmlParser::parse(const uint8_t* data, size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) return false;
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVTtmlParser::reset() {
  mImpl->reset();
}

} // namespace coral
