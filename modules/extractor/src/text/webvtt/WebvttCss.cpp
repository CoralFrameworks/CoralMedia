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

#include "text/webvtt/WebvttCss.h"

#include "text/SubtitleTextUtil.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace text {
namespace webvtt {
namespace {

struct NamedColor final {
  const char* name;
  uint32_t argb;
};

/** The CSS colour names of the reference implementation (`ColorParser.COLOR_MAP`). */
constexpr NamedColor kCssColors[] = {
  {"aliceblue", 0xFFF0F8FFu},
  {"antiquewhite", 0xFFFAEBD7u},
  {"aqua", 0xFF00FFFFu},
  {"aquamarine", 0xFF7FFFD4u},
  {"azure", 0xFFF0FFFFu},
  {"beige", 0xFFF5F5DCu},
  {"bisque", 0xFFFFE4C4u},
  {"black", 0xFF000000u},
  {"blanchedalmond", 0xFFFFEBCDu},
  {"blue", 0xFF0000FFu},
  {"blueviolet", 0xFF8A2BE2u},
  {"brown", 0xFFA52A2Au},
  {"burlywood", 0xFFDEB887u},
  {"cadetblue", 0xFF5F9EA0u},
  {"chartreuse", 0xFF7FFF00u},
  {"chocolate", 0xFFD2691Eu},
  {"coral", 0xFFFF7F50u},
  {"cornflowerblue", 0xFF6495EDu},
  {"cornsilk", 0xFFFFF8DCu},
  {"crimson", 0xFFDC143Cu},
  {"cyan", 0xFF00FFFFu},
  {"darkblue", 0xFF00008Bu},
  {"darkcyan", 0xFF008B8Bu},
  {"darkgoldenrod", 0xFFB8860Bu},
  {"darkgray", 0xFFA9A9A9u},
  {"darkgreen", 0xFF006400u},
  {"darkgrey", 0xFFA9A9A9u},
  {"darkkhaki", 0xFFBDB76Bu},
  {"darkmagenta", 0xFF8B008Bu},
  {"darkolivegreen", 0xFF556B2Fu},
  {"darkorange", 0xFFFF8C00u},
  {"darkorchid", 0xFF9932CCu},
  {"darkred", 0xFF8B0000u},
  {"darksalmon", 0xFFE9967Au},
  {"darkseagreen", 0xFF8FBC8Fu},
  {"darkslateblue", 0xFF483D8Bu},
  {"darkslategray", 0xFF2F4F4Fu},
  {"darkslategrey", 0xFF2F4F4Fu},
  {"darkturquoise", 0xFF00CED1u},
  {"darkviolet", 0xFF9400D3u},
  {"deeppink", 0xFFFF1493u},
  {"deepskyblue", 0xFF00BFFFu},
  {"dimgray", 0xFF696969u},
  {"dimgrey", 0xFF696969u},
  {"dodgerblue", 0xFF1E90FFu},
  {"firebrick", 0xFFB22222u},
  {"floralwhite", 0xFFFFFAF0u},
  {"forestgreen", 0xFF228B22u},
  {"fuchsia", 0xFFFF00FFu},
  {"gainsboro", 0xFFDCDCDCu},
  {"ghostwhite", 0xFFF8F8FFu},
  {"gold", 0xFFFFD700u},
  {"goldenrod", 0xFFDAA520u},
  {"gray", 0xFF808080u},
  {"green", 0xFF008000u},
  {"greenyellow", 0xFFADFF2Fu},
  {"grey", 0xFF808080u},
  {"honeydew", 0xFFF0FFF0u},
  {"hotpink", 0xFFFF69B4u},
  {"indianred", 0xFFCD5C5Cu},
  {"indigo", 0xFF4B0082u},
  {"ivory", 0xFFFFFFF0u},
  {"khaki", 0xFFF0E68Cu},
  {"lavender", 0xFFE6E6FAu},
  {"lavenderblush", 0xFFFFF0F5u},
  {"lawngreen", 0xFF7CFC00u},
  {"lemonchiffon", 0xFFFFFACDu},
  {"lightblue", 0xFFADD8E6u},
  {"lightcoral", 0xFFF08080u},
  {"lightcyan", 0xFFE0FFFFu},
  {"lightgoldenrodyellow", 0xFFFAFAD2u},
  {"lightgray", 0xFFD3D3D3u},
  {"lightgreen", 0xFF90EE90u},
  {"lightgrey", 0xFFD3D3D3u},
  {"lightpink", 0xFFFFB6C1u},
  {"lightsalmon", 0xFFFFA07Au},
  {"lightseagreen", 0xFF20B2AAu},
  {"lightskyblue", 0xFF87CEFAu},
  {"lightslategray", 0xFF778899u},
  {"lightslategrey", 0xFF778899u},
  {"lightsteelblue", 0xFFB0C4DEu},
  {"lightyellow", 0xFFFFFFE0u},
  {"lime", 0xFF00FF00u},
  {"limegreen", 0xFF32CD32u},
  {"linen", 0xFFFAF0E6u},
  {"magenta", 0xFFFF00FFu},
  {"maroon", 0xFF800000u},
  {"mediumaquamarine", 0xFF66CDAAu},
  {"mediumblue", 0xFF0000CDu},
  {"mediumorchid", 0xFFBA55D3u},
  {"mediumpurple", 0xFF9370DBu},
  {"mediumseagreen", 0xFF3CB371u},
  {"mediumslateblue", 0xFF7B68EEu},
  {"mediumspringgreen", 0xFF00FA9Au},
  {"mediumturquoise", 0xFF48D1CCu},
  {"mediumvioletred", 0xFFC71585u},
  {"midnightblue", 0xFF191970u},
  {"mintcream", 0xFFF5FFFAu},
  {"mistyrose", 0xFFFFE4E1u},
  {"moccasin", 0xFFFFE4B5u},
  {"navajowhite", 0xFFFFDEADu},
  {"navy", 0xFF000080u},
  {"oldlace", 0xFFFDF5E6u},
  {"olive", 0xFF808000u},
  {"olivedrab", 0xFF6B8E23u},
  {"orange", 0xFFFFA500u},
  {"orangered", 0xFFFF4500u},
  {"orchid", 0xFFDA70D6u},
  {"palegoldenrod", 0xFFEEE8AAu},
  {"palegreen", 0xFF98FB98u},
  {"paleturquoise", 0xFFAFEEEEu},
  {"palevioletred", 0xFFDB7093u},
  {"papayawhip", 0xFFFFEFD5u},
  {"peachpuff", 0xFFFFDAB9u},
  {"peru", 0xFFCD853Fu},
  {"pink", 0xFFFFC0CBu},
  {"plum", 0xFFDDA0DDu},
  {"powderblue", 0xFFB0E0E6u},
  {"purple", 0xFF800080u},
  {"rebeccapurple", 0xFF663399u},
  {"red", 0xFFFF0000u},
  {"rosybrown", 0xFFBC8F8Fu},
  {"royalblue", 0xFF4169E1u},
  {"saddlebrown", 0xFF8B4513u},
  {"salmon", 0xFFFA8072u},
  {"sandybrown", 0xFFF4A460u},
  {"seagreen", 0xFF2E8B57u},
  {"seashell", 0xFFFFF5EEu},
  {"sienna", 0xFFA0522Du},
  {"silver", 0xFFC0C0C0u},
  {"skyblue", 0xFF87CEEBu},
  {"slateblue", 0xFF6A5ACDu},
  {"slategray", 0xFF708090u},
  {"slategrey", 0xFF708090u},
  {"snow", 0xFFFFFAFAu},
  {"springgreen", 0xFF00FF7Fu},
  {"steelblue", 0xFF4682B4u},
  {"tan", 0xFFD2B48Cu},
  {"teal", 0xFF008080u},
  {"thistle", 0xFFD8BFD8u},
  {"tomato", 0xFFFF6347u},
  {"transparent", 0x00000000u},
  {"turquoise", 0xFF40E0D0u},
  {"violet", 0xFFEE82EEu},
  {"wheat", 0xFFF5DEB3u},
  {"white", 0xFFFFFFFFu},
  {"whitesmoke", 0xFFF5F5F5u},
  {"yellow", 0xFFFFFF00u},
  {"yellowgreen", 0xFF9ACD32u},
};

std::string stripQuotes(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
      value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

int hexDigit(const char character) {
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
}

bool parseHexColor(const std::string& hex, uint32_t& argb) {
  if (hex.size() != 3 && hex.size() != 6 && hex.size() != 8) {
    return false;
  }
  uint32_t value = 0;
  for (const char character: hex) {
    const int digit = hexDigit(character);
    if (digit < 0) {
      return false;
    }
    value = (value << 4) | static_cast<uint32_t>(digit);
  }
  if (hex.size() == 3) {
    const uint32_t red = (value >> 8) & 0xFu;
    const uint32_t green = (value >> 4) & 0xFu;
    const uint32_t blue = value & 0xFu;
    argb = 0xFF000000u | (red << 20) | (red << 16) | (green << 12) | (green << 8) | (blue << 4) |
           blue;
    return true;
  }
  if (hex.size() == 6) {
    argb = 0xFF000000u | value;
    return true;
  }
  // #RRGGBBAA, which the parser turns into ARGB.
  const uint32_t alpha = value & 0xFFu;
  argb = (alpha << 24) | ((value >> 8) & 0xFFFFFFu);
  return true;
}

bool parseRgbFunction(const std::string& value, uint32_t& argb) {
  if (value.rfind("rgb(", 0) != 0 && value.rfind("rgba(", 0) != 0) {
    return false;
  }
  const size_t open = value.find('(');
  const size_t close = value.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open) {
    return false;
  }
  const std::vector<std::string> parts = split(
    std::string_view(value).substr(open + 1, close - open - 1), ',');
  if (parts.size() != 3 && parts.size() != 4) {
    return false;
  }
  int channels[3] = {0, 0, 0};
  for (size_t index = 0; index < 3; ++index) {
    int64_t channel = 0;
    if (!parseInteger(trim(parts[index]), channel)) {
      return false;
    }
    channels[index] = static_cast<int>(std::clamp<int64_t>(channel, 0, 255));
  }
  uint32_t alpha = 255;
  if (parts.size() == 4) {
    float floatAlpha = 0.0f;
    int64_t intAlpha = 0;
    const std::string alphaText = trim(parts[3]);
    if (parseFloat(alphaText, floatAlpha) && alphaText.find('.') != std::string::npos) {
      alpha = static_cast<uint32_t>(std::clamp(floatAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    } else if (parseInteger(alphaText, intAlpha)) {
      alpha = static_cast<uint32_t>(std::clamp<int64_t>(intAlpha, 0, 255));
    } else {
      return false;
    }
  }
  argb = (alpha << 24) | (static_cast<uint32_t>(channels[0]) << 16) |
         (static_cast<uint32_t>(channels[1]) << 8) | static_cast<uint32_t>(channels[2]);
  return true;
}

bool parseCssColor(const std::string& expression, uint32_t& argb) {
  const std::string value = toLowerAscii(trim(expression));
  if (value.empty()) {
    return false;
  }
  if (value.front() == '#') {
    return parseHexColor(value.substr(1), argb);
  }
  if (parseRgbFunction(value, argb)) {
    return true;
  }
  for (const auto& color: kCssColors) {
    if (value == color.name) {
      argb = color.argb;
      return true;
    }
  }
  return false;
}

bool parseFontSize(const std::string& expression, CssStyle& style) {
  const std::string value = toLowerAscii(trim(expression));
  CssFontSizeUnit unit = CssFontSizeUnit::kUnset;
  size_t unitLength = 0;
  if (value.size() > 2 && value.compare(value.size() - 2, 2, "px") == 0) {
    unit = CssFontSizeUnit::kPixels;
    unitLength = 2;
  } else if (value.size() > 2 && value.compare(value.size() - 2, 2, "em") == 0) {
    unit = CssFontSizeUnit::kEm;
    unitLength = 2;
  } else if (!value.empty() && value.back() == '%') {
    unit = CssFontSizeUnit::kPercent;
    unitLength = 1;
  }
  if (unit == CssFontSizeUnit::kUnset) {
    return false;
  }
  float size = 0.0f;
  if (!parseFloat(trim(std::string_view(value).substr(0, value.size() - unitLength)), size) ||
      size <= 0.0f) {
    return false;
  }
  style.fontSizeUnit = unit;
  style.fontSize = size;
  return true;
}

bool parseSelector(const std::string& selector, CssStyle& style) {
  std::string text = trim(selector);
  if (text == "::cue") {
    return true;
  }
  if (text.rfind("::cue(", 0) != 0 || text.back() != ')') {
    return false;
  }
  std::string target = trim(std::string_view(text).substr(6, text.size() - 7));
  const size_t bracket = target.find('[');
  if (bracket != std::string::npos) {
    const std::string voicePart = target.substr(bracket);
    const size_t equals = voicePart.find('=');
    if (equals != std::string::npos) {
      std::string voice = voicePart.substr(equals + 1);
      voice = trim(voice);
      if (!voice.empty() && voice.back() == ']') {
        voice.pop_back();
      }
      style.targetVoice = stripQuotes(voice);
    }
    target = trim(target.substr(0, bracket));
  }
  if (target.empty()) {
    return true;
  }
  std::vector<std::string> parts = split(target, '.');
  std::string tagAndId = trim(parts.front());
  const size_t hash = tagAndId.find('#');
  if (hash != std::string::npos) {
    style.targetTag = trim(tagAndId.substr(0, hash));
    style.targetId = trim(tagAndId.substr(hash + 1));
  } else {
    style.targetTag = tagAndId;
  }
  for (size_t index = 1; index < parts.size(); ++index) {
    const std::string className = trim(parts[index]);
    if (!className.empty()) {
      style.targetClasses.push_back(className);
    }
  }
  return true;
}

void applyDeclaration(const std::string& name, const std::string& value, CssStyle& style) {
  if (name == "color") {
    style.hasColor = parseCssColor(value, style.color);
  } else if (name == "background-color") {
    style.hasBackgroundColor = parseCssColor(value, style.backgroundColor);
  } else if (name == "font-family") {
    style.fontFamily = stripQuotes(value);
    style.hasFontFamily = !style.fontFamily.empty();
  } else if (name == "font-weight") {
    const std::string lowered = toLowerAscii(trim(value));
    if (lowered == "bold") {
      style.hasBold = true;
      style.bold = true;
    } else if (lowered == "normal") {
      style.hasBold = true;
      style.bold = false;
    }
  } else if (name == "font-style") {
    const std::string lowered = toLowerAscii(trim(value));
    if (lowered == "italic") {
      style.hasItalic = true;
      style.italic = true;
    } else if (lowered == "normal") {
      style.hasItalic = true;
      style.italic = false;
    }
  } else if (name == "text-decoration") {
    const std::string lowered = toLowerAscii(trim(value));
    if (lowered.find("underline") != std::string::npos) {
      style.hasUnderline = true;
      style.underline = true;
    }
    if (lowered.find("line-through") != std::string::npos) {
      style.hasLineThrough = true;
      style.lineThrough = true;
    }
  } else if (name == "font-size") {
    parseFontSize(value, style);
  } else if (name == "ruby-position") {
    const std::string lowered = toLowerAscii(trim(value));
    if (lowered == "over") {
      style.hasRubyPosition = true;
      style.rubyPosition = AVCueSpan::RubyPosition::kBefore;
    } else if (lowered == "under") {
      style.hasRubyPosition = true;
      style.rubyPosition = AVCueSpan::RubyPosition::kAfter;
    }
  } else if (name == "text-combine-upright") {
    const std::string lowered = toLowerAscii(trim(value));
    if (lowered == "all" || lowered.rfind("digits", 0) == 0) {
      style.hasCombineUpright = true;
      style.combineUpright = true;
    }
  }
}

std::string stripComments(const std::string& block) {
  std::string text;
  bool inComment = false;
  for (size_t i = 0; i < block.size(); ++i) {
    if (!inComment && block.compare(i, 2, "/*") == 0) {
      inComment = true;
      ++i;
      continue;
    }
    if (inComment) {
      if (block.compare(i, 2, "*/") == 0) {
        inComment = false;
        ++i;
      }
      continue;
    }
    text.push_back(block[i]);
  }
  return text;
}

} // namespace

std::vector<CssStyle> parseCssStyles(const std::string& block) {
  const std::string text = stripComments(block);
  std::vector<CssStyle> styles;
  size_t position = 0;
  while (position < text.size()) {
    const size_t open = text.find('{', position);
    if (open == std::string::npos) {
      break;
    }
    const size_t close = text.find('}', open + 1);
    if (close == std::string::npos) {
      break;
    }
    CssStyle style;
    if (!parseSelector(text.substr(position, open - position), style)) {
      position = close + 1;
      continue;
    }
    const std::string declarations = text.substr(open + 1, close - open - 1);
    for (const auto& declaration: split(declarations, ';')) {
      const size_t colon = declaration.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      applyDeclaration(toLowerAscii(trim(std::string_view(declaration).substr(0, colon))),
        std::string(std::string_view(declaration).substr(colon + 1)), style);
    }
    style.order = styles.size();
    styles.push_back(std::move(style));
    position = close + 1;
  }
  return styles;
}

int cssSpecificity(const CssStyle& style, const std::string& cueId, const std::string& tag,
  const std::vector<std::string>& classes, const std::string& voice) {
  const bool universal = style.targetId.empty() && style.targetTag.empty() &&
                         style.targetClasses.empty() && style.targetVoice.empty();
  if (universal) {
    // A universal rule is the default of the whole cue only: media3 gives it the minimum score for
    // the empty whole-cue element and no match for a real element.
    return tag.empty() ? 1 : 0;
  }
  int score = 0;
  const auto match = [&score](const std::string& target, const std::string& actual, const int weight) {
    if (target.empty() || score < 0) {
      return;
    }
    score = target == actual ? score + weight : -1;
  };
  match(style.targetId, cueId, 0x40000000);
  match(style.targetTag, tag, 2);
  match(style.targetVoice, voice, 4);
  if (score < 0) {
    return 0;
  }
  for (const auto& wanted: style.targetClasses) {
    if (std::find(classes.begin(), classes.end(), wanted) == classes.end()) {
      return 0;
    }
  }
  return score + static_cast<int>(style.targetClasses.size()) * 4;
}

} // namespace webvtt
} // namespace text
} // namespace coral
