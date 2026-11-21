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

#include "WebvttCueParser.h"

#include "text/SubtitleTextUtil.h"
#include "text/webvtt/WebvttCss.h"

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

constexpr float kDefaultPosition = 0.5f;

/** The default text colours of the WebVTT specification, as ARGB. */
struct DefaultColor final {
  const char* name;
  uint32_t argb;
};

constexpr DefaultColor kDefaultTextColors[] = {
  {"white", 0xFFFFFFFFu}, {"lime", 0xFF00FF00u}, {"cyan", 0xFF00FFFFu}, {"red", 0xFFFF0000u},
  {"yellow", 0xFFFFFF00u}, {"magenta", 0xFFFF00FFu}, {"blue", 0xFF0000FFu}, {"black", 0xFF000000u},
};

constexpr DefaultColor kDefaultBackgroundColors[] = {
  {"bg_white", 0xFFFFFFFFu}, {"bg_lime", 0xFF00FF00u}, {"bg_cyan", 0xFF00FFFFu},
  {"bg_red", 0xFFFF0000u},   {"bg_yellow", 0xFFFFFF00u}, {"bg_magenta", 0xFFFF00FFu},
  {"bg_blue", 0xFF0000FFu},  {"bg_black", 0xFF000000u},
};

/** One element of the tag stack of a cue being parsed. */
struct OpenTag final {
  std::string name;
  std::string voice;
  std::string language;
  std::vector<std::string> classes;
  /** The byte offset of the text the element covers, in the text built so far. */
  size_t start = 0;
  /** The byte offset where the base text of the ruby started, for a ruby element. */
  size_t rubyBaseStart = 0;
};

/** The text range an element covered, kept so a style sheet can target it after parsing. */
struct TagRange final {
  std::string name;
  std::vector<std::string> classes;
  std::string voice;
  size_t start = 0;
  size_t end = 0;
};

/** The parser of the markup of one cue. */
class MarkupParser final {
public:
  MarkupParser() = default;

  MarkupParser(const std::vector<CssStyle>& styles, std::string cueId)
      : mStyles(&styles), mCueId(std::move(cueId)) {
  }

  void parse(const std::string_view markup, AVCue& cue) {
    size_t position = 0;
    while (position < markup.size()) {
      const char character = markup[position];
      if (character == '<') {
        const size_t tagEnd = markup.find('>', position + 1);
        if (tagEnd == std::string_view::npos) {
          appendText(markup.substr(position));
          break;
        }
        const bool closing = markup[position + 1] == '/';
        size_t expressionStart = position + (closing ? 2 : 1);
        size_t expressionSize = tagEnd - expressionStart;
        std::string_view expression = markup.substr(expressionStart, expressionSize);
        const bool selfClosing = !expression.empty() && expression.back() == '/';
        if (selfClosing) {
          expression = expression.substr(0, expression.size() - 1);
        }
        const std::string name = tagName(expression);
        if (isSupportedTag(name)) {
          if (closing) {
            closeTag(name);
          } else if (!selfClosing) {
            openTag(expression, name);
          }
        }
        position = tagEnd + 1;
      } else if (character == '&') {
        position = handleEntity(markup, position);
      } else {
        position = appendCodePoint(markup, position);
      }
    }

    // The elements that were never closed cover the end of the cue.
    while (!mStack.empty()) {
      applyTag(mStack.back(), mText.size());
      mStack.pop_back();
    }
    applyStyleSheet();
    cue.text = std::move(mText);
    cue.spans = std::move(mSpans);
  }

private:
  static std::string tagName(const std::string_view expression) {
    const std::string trimmed = trim(expression);
    if (trimmed.empty()) {
      return {};
    }
    const size_t end = trimmed.find_first_of(" .");
    return end == std::string::npos ? trimmed : trimmed.substr(0, end);
  }

  static bool isSupportedTag(const std::string& name) {
    return name == "b" || name == "i" || name == "u" || name == "s" || name == "c" ||
           name == "v" || name == "lang" || name == "ruby" || name == "rt";
  }

  void openTag(const std::string_view expression, const std::string& name) {
    std::string rest = trim(expression);
    OpenTag tag;
    tag.name = name;
    tag.start = mText.size();
    tag.rubyBaseStart = tag.start;
    if (name == "v" || name == "lang") {
      const size_t separator = rest.find(' ');
      if (separator != std::string::npos) {
        const std::string value = trim(std::string_view(rest).substr(separator + 1));
        if (name == "v") {
          tag.voice = value;
        } else {
          tag.language = value;
        }
        rest = rest.substr(0, separator);
      }
    }
    // The classes follow the element name, separated by dots: `<c.yellow.bg_blue>`.
    const std::vector<std::string> parts = split(rest, '.');
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string className = trim(parts[i]);
      if (!className.empty()) {
        tag.classes.push_back(className);
      }
    }
    mStack.push_back(std::move(tag));
  }

  void closeTag(const std::string& name) {
    if (name == "rt") {
      closeRubyText();
      return;
    }
    if (name == "ruby") {
      // The annotations were attached when their `<rt>` elements closed; the element itself only
      // ends the base text.
      for (auto it = mStack.rbegin(); it != mStack.rend(); ++it) {
        if (it->name == "ruby") {
          while (!mStack.empty()) {
            OpenTag tag = std::move(mStack.back());
            mStack.pop_back();
            if (tag.name == "ruby") {
              return;
            }
            applyTag(tag, mText.size());
          }
          return;
        }
      }
      return;
    }

    // Pop until the matching element; every element that is popped ends here, which is how the
    // markup of a document that overlaps two elements is resolved.
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

  void closeRubyText() {
    size_t rtIndex = std::string::npos;
    for (size_t i = mStack.size(); i > 0; --i) {
      if (mStack[i - 1].name == "rt") {
        rtIndex = i - 1;
        break;
      }
    }
    if (rtIndex == std::string::npos) {
      return;
    }
    // Elements opened inside the annotation decorate text that is about to be removed.
    while (mStack.size() > rtIndex + 1) {
      mStack.pop_back();
    }
    size_t rubyIndex = std::string::npos;
    for (size_t i = rtIndex; i > 0; --i) {
      if (mStack[i - 1].name == "ruby") {
        rubyIndex = i - 1;
        break;
      }
    }
    const OpenTag rt = std::move(mStack.back());
    mStack.pop_back();
    if (rt.start >= mText.size()) {
      return;
    }
    const std::string annotation = mText.substr(rt.start, mText.size() - rt.start);
    if (rubyIndex == std::string::npos) {
      eraseRange(rt.start, mText.size());
      return;
    }
    OpenTag& ruby = mStack[rubyIndex];
    if (rt.start > ruby.rubyBaseStart) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kRuby;
      span.start = ruby.rubyBaseStart;
      span.end = rt.start;
      span.ruby.text = annotation;
      span.ruby.type = AVCueSpan::RubyType::kBase;
      span.ruby.position = rubyPositionFor(rt, ruby);
      mSpans.push_back(std::move(span));
    }
    eraseRange(rt.start, mText.size());
    ruby.rubyBaseStart = rt.start;
  }

  void applyTag(const OpenTag& tag, const size_t end) {
    if (end <= tag.start) {
      return;
    }
    // Kept for the style sheet: a rule can target the element by name, class or voice, and the
    // cascade is resolved once the whole cue is parsed.
    mRanges.push_back({tag.name, tag.classes, tag.voice, tag.start, end});
    AVCueSpan span;
    span.start = tag.start;
    span.end = end;
    if (tag.name == "b") {
      span.type = AVCueSpan::Type::kTextStyle;
      span.style.bold = true;
    } else if (tag.name == "i") {
      span.type = AVCueSpan::Type::kTextStyle;
      span.style.italic = true;
    } else if (tag.name == "u") {
      span.type = AVCueSpan::Type::kTextStyle;
      span.style.underline = true;
    } else if (tag.name == "s") {
      span.type = AVCueSpan::Type::kTextStyle;
      span.style.strikeout = true;
    } else if (tag.name == "v") {
      if (tag.voice.empty()) {
        return;
      }
      span.type = AVCueSpan::Type::kVoice;
      span.text = tag.voice;
    } else if (tag.name == "lang") {
      if (tag.language.empty()) {
        return;
      }
      span.type = AVCueSpan::Type::kLanguage;
      span.text = tag.language;
    } else if (tag.name == "c") {
      // A class that names a default colour decorates the text with it. The classes that name
      // neither colour are the ones of a stylesheet, which this parser does not evaluate.
      for (const auto& className : tag.classes) {
        for (const auto& color : kDefaultTextColors) {
          if (className == color.name) {
            AVCueSpan colorSpan;
            colorSpan.type = AVCueSpan::Type::kForegroundColor;
            colorSpan.start = tag.start;
            colorSpan.end = end;
            colorSpan.color = color.argb;
            mSpans.push_back(std::move(colorSpan));
          }
        }
        for (const auto& color : kDefaultBackgroundColors) {
          if (className == color.name) {
            AVCueSpan colorSpan;
            colorSpan.type = AVCueSpan::Type::kBackgroundColor;
            colorSpan.start = tag.start;
            colorSpan.end = end;
            colorSpan.color = color.argb;
            mSpans.push_back(std::move(colorSpan));
          }
        }
      }
      return;
    } else {
      return;
    }
    mSpans.push_back(std::move(span));
  }

  /**
   * @return the ruby position a style sheet asks for the annotation of @p rt inside @p ruby.
   *
   * <p>The `<rt>` element wins over the `<ruby>` element, and `over` is the default, which is what
   * the reference implementation does (`firstKnownRubyPosition`).
   */
  AVCueSpan::RubyPosition rubyPositionFor(const OpenTag& rt, const OpenTag& ruby) const {
    if (mStyles == nullptr || mStyles->empty()) {
      return AVCueSpan::RubyPosition::kBefore;
    }
    int bestScore = 0;
    AVCueSpan::RubyPosition best = AVCueSpan::RubyPosition::kUnknown;
    for (const auto& style: *mStyles) {
      if (!style.hasRubyPosition) continue;
      const int score = cssSpecificity(style, mCueId, rt.name, rt.classes, rt.voice);
      if (score > bestScore) {
        bestScore = score;
        best = style.rubyPosition;
      }
    }
    if (bestScore > 0) {
      return best;
    }
    for (const auto& style: *mStyles) {
      if (!style.hasRubyPosition) continue;
      const int score = cssSpecificity(style, mCueId, ruby.name, ruby.classes, ruby.voice);
      if (score > bestScore) {
        bestScore = score;
        best = style.rubyPosition;
      }
    }
    return bestScore > 0 ? best : AVCueSpan::RubyPosition::kBefore;
  }

  /**
   * Resolves the style sheet over the ranges of the cue.
   *
   * <p>Every range is matched against every rule; the declarations of the matching rules are merged
   * per property with the highest specificity first (the later rule wins at equal specificity), and
   * the resulting spans are inserted at the front of the cue's spans, in ascending order of the
   * range's best score. A renderer that takes the first covering span therefore sees the highest
   * specificity, and `::cue` acts as the default of the whole cue.
   */
  void applyStyleSheet() {
    if (mStyles == nullptr || mStyles->empty() || mText.empty()) {
      return;
    }
    struct Match final {
      int score = 0;
      size_t order = 0;
      const CssStyle* style = nullptr;
    };
    struct RangePlan final {
      size_t start = 0;
      size_t end = 0;
      int bestScore = 0;
      std::vector<Match> matches;
    };
    std::vector<RangePlan> plans;
    for (const auto& range: mRanges) {
      RangePlan plan;
      plan.start = range.start;
      plan.end = range.end;
      for (const auto& style: *mStyles) {
        const int score = cssSpecificity(style, mCueId, range.name, range.classes, range.voice);
        if (score > 0) {
          plan.matches.push_back({score, style.order, &style});
          plan.bestScore = std::max(plan.bestScore, score);
        }
      }
      if (!plan.matches.empty()) {
        plans.push_back(std::move(plan));
      }
    }
    // `::cue` targets the whole cue, which is the pseudo element with an empty name.
    {
      RangePlan plan;
      plan.start = 0;
      plan.end = mText.size();
      for (const auto& style: *mStyles) {
        const int score = cssSpecificity(style, mCueId, {}, {}, {});
        if (score > 0) {
          plan.matches.push_back({score, style.order, &style});
          plan.bestScore = std::max(plan.bestScore, score);
        }
      }
      if (!plan.matches.empty()) {
        plans.push_back(std::move(plan));
      }
    }
    std::sort(plans.begin(), plans.end(), [](const RangePlan& left, const RangePlan& right) {
      return left.bestScore < right.bestScore;
    });

    for (auto& plan: plans) {
      std::sort(plan.matches.begin(), plan.matches.end(), [](const Match& left, const Match& right) {
        if (left.score != right.score) {
          return left.score > right.score;
        }
        return left.order > right.order;
      });
      bool hasColor = false;
      uint32_t color = 0;
      bool hasBackground = false;
      uint32_t background = 0;
      bool hasBold = false;
      bool bold = false;
      bool hasItalic = false;
      bool italic = false;
      bool hasUnderline = false;
      bool underline = false;
      bool hasLineThrough = false;
      bool lineThrough = false;
      bool hasFontFamily = false;
      std::string fontFamily;
      bool hasFontSize = false;
      CssFontSizeUnit fontSizeUnit = CssFontSizeUnit::kUnset;
      float fontSize = 0.0f;
      bool hasCombineUpright = false;
      bool combineUpright = false;
      for (const auto& match: plan.matches) {
        const CssStyle& style = *match.style;
        if (!hasColor && style.hasColor) {
          hasColor = true;
          color = style.color;
        }
        if (!hasBackground && style.hasBackgroundColor) {
          hasBackground = true;
          background = style.backgroundColor;
        }
        if (!hasBold && style.hasBold) {
          hasBold = true;
          bold = style.bold;
        }
        if (!hasItalic && style.hasItalic) {
          hasItalic = true;
          italic = style.italic;
        }
        if (!hasUnderline && style.hasUnderline) {
          hasUnderline = true;
          underline = style.underline;
        }
        if (!hasLineThrough && style.hasLineThrough) {
          hasLineThrough = true;
          lineThrough = style.lineThrough;
        }
        if (!hasFontFamily && style.hasFontFamily) {
          hasFontFamily = true;
          fontFamily = style.fontFamily;
        }
        if (!hasFontSize && style.fontSizeUnit != CssFontSizeUnit::kUnset) {
          hasFontSize = true;
          fontSizeUnit = style.fontSizeUnit;
          fontSize = style.fontSize;
        }
        if (!hasCombineUpright && style.hasCombineUpright) {
          hasCombineUpright = true;
          combineUpright = style.combineUpright;
        }
      }

      std::vector<AVCueSpan> spans;
      if (hasColor) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kForegroundColor;
        span.start = plan.start;
        span.end = plan.end;
        span.color = color;
        spans.push_back(std::move(span));
      }
      if (hasBackground) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kBackgroundColor;
        span.start = plan.start;
        span.end = plan.end;
        span.color = background;
        spans.push_back(std::move(span));
      }
      if (hasCombineUpright && combineUpright) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kHorizontalTextInVerticalContext;
        span.start = plan.start;
        span.end = plan.end;
        spans.push_back(std::move(span));
      }
      if (hasBold || hasItalic || hasUnderline || hasLineThrough || hasFontFamily || hasFontSize) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kTextStyle;
        span.start = plan.start;
        span.end = plan.end;
        span.style.bold = hasBold && bold;
        span.style.italic = hasItalic && italic;
        span.style.underline = hasUnderline && underline;
        span.style.strikeout = hasLineThrough && lineThrough;
        if (hasFontFamily) {
          span.style.fontFamily = fontFamily;
        }
        if (hasFontSize) {
          span.style.textSize =
            fontSizeUnit == CssFontSizeUnit::kPercent ? fontSize / 100.0f : fontSize;
          span.style.textSizeAbsolute = fontSizeUnit == CssFontSizeUnit::kPixels;
        }
        spans.push_back(std::move(span));
      }
      if (!spans.empty()) {
        mSpans.insert(mSpans.begin(), spans.begin(), spans.end());
      }
    }
  }

  size_t handleEntity(const std::string_view markup, const size_t position) {
    const size_t semicolon = markup.find(';', position + 1);
    const size_t space = markup.find(' ', position + 1);
    if (semicolon == std::string_view::npos && space == std::string_view::npos) {
      appendText("&");
      return position + 1;
    }
    const bool endsAtSemicolon =
      semicolon != std::string_view::npos && (space == std::string_view::npos || semicolon < space);
    const size_t end = endsAtSemicolon ? semicolon : space;
    applyEntity(markup.substr(position + 1, end - position - 1));
    if (!endsAtSemicolon) {
      appendText(" ");
    }
    return end + 1;
  }

  void applyEntity(const std::string_view entity) {
    if (entity == "lt") {
      appendText("<");
    } else if (entity == "gt") {
      appendText(">");
    } else if (entity == "amp") {
      appendText("&");
    } else if (entity == "nbsp") {
      appendText(" ");
    } else if (entity == "lrm") {
      appendText("\xE2\x80\x8E");
    } else if (entity == "rlm") {
      appendText("\xE2\x80\x8F");
    }
    // Anything else is dropped, as the reference implementation does: the specification only
    // defines the four entities above plus the two directional marks.
  }

  void appendText(const std::string_view text) { mText.append(text.data(), text.size()); }

  static size_t codePointLength(const unsigned char lead) {
    if (lead < 0x80) {
      return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
      return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
      return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
      return 4;
    }
    return 1;
  }

  size_t appendCodePoint(const std::string_view markup, const size_t position) {
    const size_t length = codePointLength(static_cast<unsigned char>(markup[position]));
    const size_t available = markup.size() - position;
    const size_t copied = std::min(length, available);
    appendText(markup.substr(position, copied));
    return position + copied;
  }

  /** Removes [begin, end) from the text, and moves the spans and elements that follow it. */
  void eraseRange(const size_t begin, const size_t end) {
    if (end <= begin || end > mText.size()) {
      return;
    }
    const size_t removed = end - begin;
    mText.erase(begin, removed);
    std::vector<AVCueSpan> kept;
    kept.reserve(mSpans.size());
    for (auto& span : mSpans) {
      if (span.end <= begin) {
        kept.push_back(std::move(span));
      } else if (span.start >= end) {
        span.start -= removed;
        span.end -= removed;
        kept.push_back(std::move(span));
      } else {
        span.start = std::min(span.start, begin);
        span.end = span.end > end ? span.end - removed : span.start;
        if (span.end > span.start) {
          kept.push_back(std::move(span));
        }
      }
    }
    mSpans = std::move(kept);
    for (auto& tag : mStack) {
      if (tag.start >= end) {
        tag.start -= removed;
        if (tag.rubyBaseStart >= end) {
          tag.rubyBaseStart -= removed;
        }
      } else if (tag.start > begin) {
        tag.start = begin;
      }
    }
    for (auto& range: mRanges) {
      if (range.end <= begin) {
        continue;
      }
      if (range.start >= end) {
        range.start -= removed;
        range.end -= removed;
      } else {
        range.start = std::min(range.start, begin);
        range.end = range.end > end ? range.end - removed : range.start;
      }
    }
    mRanges.erase(std::remove_if(mRanges.begin(), mRanges.end(),
                    [](const TagRange& range) { return range.end <= range.start; }),
      mRanges.end());
  }

  std::string mText;
  std::vector<AVCueSpan> mSpans;
  std::vector<OpenTag> mStack;
  std::vector<TagRange> mRanges;
  const std::vector<CssStyle>* mStyles = nullptr;
  std::string mCueId;
};

float derivePosition(const AVCue::Alignment alignment) {
  switch (alignment) {
    case AVCue::Alignment::kLeft:
      return 0.0f;
    case AVCue::Alignment::kRight:
      return 1.0f;
    case AVCue::Alignment::kStart:
    case AVCue::Alignment::kCenter:
    case AVCue::Alignment::kEnd:
    default:
      return kDefaultPosition;
  }
}

AVCue::AnchorType derivePositionAnchor(const AVCue::Alignment alignment) {
  switch (alignment) {
    case AVCue::Alignment::kLeft:
    case AVCue::Alignment::kStart:
      return AVCue::AnchorType::kStart;
    case AVCue::Alignment::kRight:
    case AVCue::Alignment::kEnd:
      return AVCue::AnchorType::kEnd;
    case AVCue::Alignment::kCenter:
    case AVCue::Alignment::kUnset:
    default:
      return AVCue::AnchorType::kMiddle;
  }
}

float deriveMaxSize(const AVCue::AnchorType positionAnchor, const float position) {
  switch (positionAnchor) {
    case AVCue::AnchorType::kStart:
      return 1.0f - position;
    case AVCue::AnchorType::kEnd:
      return position;
    case AVCue::AnchorType::kMiddle:
      return position <= 0.5f ? position * 2 : (1.0f - position) * 2;
    case AVCue::AnchorType::kUnset:
    default:
      return 1.0f;
  }
}

float computeLine(const float line, const AVCue::LineType lineType) {
  if (line != AVCue::kDimenUnset && lineType == AVCue::LineType::kFraction &&
      (line < 0.0f || line > 1.0f)) {
    return 1.0f;
  }
  if (line != AVCue::kDimenUnset) {
    return line;
  }
  if (lineType == AVCue::LineType::kFraction) {
    return 1.0f;
  }
  return AVCue::kDimenUnset;
}

AVCue::AnchorType parseLineAnchor(const std::string_view value) {
  if (value == "start") {
    return AVCue::AnchorType::kStart;
  }
  if (value == "center" || value == "middle") {
    return AVCue::AnchorType::kMiddle;
  }
  if (value == "end") {
    return AVCue::AnchorType::kEnd;
  }
  return AVCue::AnchorType::kUnset;
}

AVCue::AnchorType parsePositionAnchor(const std::string_view value) {
  if (value == "line-left" || value == "start") {
    return AVCue::AnchorType::kStart;
  }
  if (value == "center" || value == "middle") {
    return AVCue::AnchorType::kMiddle;
  }
  if (value == "line-right" || value == "end") {
    return AVCue::AnchorType::kEnd;
  }
  return AVCue::AnchorType::kUnset;
}

AVCue::Alignment parseTextAlignment(const std::string_view value) {
  if (value == "start") {
    return AVCue::Alignment::kStart;
  }
  if (value == "left") {
    return AVCue::Alignment::kLeft;
  }
  if (value == "center" || value == "middle") {
    return AVCue::Alignment::kCenter;
  }
  if (value == "end") {
    return AVCue::Alignment::kEnd;
  }
  if (value == "right") {
    return AVCue::Alignment::kRight;
  }
  return AVCue::Alignment::kCenter;
}

AVCue::VerticalType parseVerticalType(const std::string_view value) {
  if (value == "rl") {
    return AVCue::VerticalType::kRightToLeft;
  }
  if (value == "lr") {
    return AVCue::VerticalType::kLeftToRight;
  }
  return AVCue::VerticalType::kUnset;
}

} // namespace

void applyCueDefaults(AVCue& cue) {
  cue.textAlignment = AVCue::Alignment::kCenter;
  cue.line = AVCue::kDimenUnset;
  cue.lineType = AVCue::LineType::kNumber;
  cue.lineAnchor = AVCue::AnchorType::kStart;
  cue.position = AVCue::kDimenUnset;
  cue.positionAnchor = AVCue::AnchorType::kUnset;
  cue.size = 1.0f;
  cue.verticalType = AVCue::VerticalType::kUnset;
}

bool parseCueHeaderLine(const std::string_view line, int64_t& startUs, int64_t& endUs,
                        std::string& settings) {
  const std::string trimmed = trim(line);
  const size_t arrow = trimmed.find("-->");
  if (arrow == std::string::npos) {
    return false;
  }
  const std::string startText = trim(std::string_view(trimmed).substr(0, arrow));
  const std::string rest = trim(std::string_view(trimmed).substr(arrow + 3));
  const size_t firstSpace = rest.find_first_of(" \t");
  const std::string endText = trim(firstSpace == std::string::npos
                                     ? std::string_view(rest)
                                     : std::string_view(rest).substr(0, firstSpace));
  if (!parseWebvttTimestamp(startText, startUs) || !parseWebvttTimestamp(endText, endUs) ||
      endUs < startUs) {
    return false;
  }
  settings = firstSpace == std::string::npos ? std::string()
                                             : trim(std::string_view(rest).substr(firstSpace));
  return true;
}

void applyCueSettings(const std::string_view settings, AVCue& cue) {
  // The settings are separated by spaces and each is `name:value`; the value may itself hold a
  // comma, e.g. `position:10%,line-left`.
  size_t position = 0;
  while (position < settings.size()) {
    while (position < settings.size() && (settings[position] == ' ' || settings[position] == '\t' ||
                                         settings[position] == '\f')) {
      ++position;
    }
    const size_t start = position;
    while (position < settings.size() && settings[position] != ' ' && settings[position] != '\t' &&
           settings[position] != '\f') {
      ++position;
    }
    if (position <= start) {
      break;
    }
    const std::string_view setting = settings.substr(start, position - start);
    const size_t colon = setting.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::string_view name = setting.substr(0, colon);
    const std::string_view value = setting.substr(colon + 1);
    if (name == "line") {
      const size_t comma = value.find(',');
      const std::string_view lineValue = comma == std::string_view::npos
                                           ? value
                                           : value.substr(0, comma);
      if (comma != std::string_view::npos) {
        cue.lineAnchor = parseLineAnchor(value.substr(comma + 1));
      }
      float fraction = 0.0f;
      int64_t number = 0;
      if (parsePercentage(lineValue, fraction)) {
        cue.line = fraction;
        cue.lineType = AVCue::LineType::kFraction;
      } else if (parseInteger(lineValue, number)) {
        cue.line = static_cast<float>(number);
        cue.lineType = AVCue::LineType::kNumber;
      }
    } else if (name == "align") {
      cue.textAlignment = parseTextAlignment(value);
    } else if (name == "position") {
      const size_t comma = value.find(',');
      const std::string_view positionValue = comma == std::string_view::npos
                                               ? value
                                               : value.substr(0, comma);
      if (comma != std::string_view::npos) {
        cue.positionAnchor = parsePositionAnchor(value.substr(comma + 1));
      }
      float fraction = 0.0f;
      if (parsePercentage(positionValue, fraction)) {
        cue.position = fraction;
      }
    } else if (name == "size") {
      float fraction = 0.0f;
      if (parsePercentage(value, fraction)) {
        cue.size = fraction;
      }
    } else if (name == "vertical") {
      cue.verticalType = parseVerticalType(value);
    }
  }
}

void finalizeCue(AVCue& cue) {
  const float position =
    cue.position != AVCue::kDimenUnset ? cue.position : derivePosition(cue.textAlignment);
  const AVCue::AnchorType positionAnchor =
    cue.positionAnchor != AVCue::AnchorType::kUnset ? cue.positionAnchor
                                                    : derivePositionAnchor(cue.textAlignment);
  cue.position = position;
  cue.positionAnchor = positionAnchor;
  const float maxSize = deriveMaxSize(positionAnchor, position);
  if (cue.size == AVCue::kDimenUnset || cue.size > maxSize) {
    cue.size = maxSize;
  }
  cue.line = computeLine(cue.line, cue.lineType);
}

void parseCueText(const std::string_view markup, AVCue& cue) {
  MarkupParser parser;
  parser.parse(markup, cue);
}

void parseCueText(const std::string_view markup, AVCue& cue,
  const std::vector<CssStyle>& styles, const std::string& cueId) {
  MarkupParser parser(styles, cueId);
  parser.parse(markup, cue);
}

} // namespace webvtt
} // namespace text
} // namespace coral
