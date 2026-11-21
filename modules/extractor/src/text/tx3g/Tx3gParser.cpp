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

#include <extractor/text/tx3g/Tx3gParser.h>

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

constexpr uint32_t kTypeStyl = 0x7374796C;   // 'styl'
constexpr uint32_t kTypeTbox = 0x74626F78;   // 'tbox'
constexpr uint32_t kTypeFtab = 0x66746162;   // 'ftab'

constexpr int kFontFaceBold = 0x0001;
constexpr int kFontFaceItalic = 0x0002;
constexpr int kFontFaceUnderline = 0x0004;

constexpr int kDefaultFontFace = 0;
constexpr uint32_t kDefaultTextColor = 0xFFFFFFFFu;
constexpr float kDefaultVerticalPlacement = 0.85f;
/** The style records of a sample index characters, so the text is measured in UTF-16 code units. */
constexpr uint32_t kMaxStyleRecordCount = 4096;

/** Reads a big endian value from a buffer, or returns 0 when it does not fit. */
uint16_t readUInt16(const uint8_t* data, const size_t size, const size_t offset) {
  if (offset + 2 > size) {
    return 0;
  }
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

uint32_t readUInt32(const uint8_t* data, const size_t size, const size_t offset) {
  if (offset + 4 > size) {
    return 0;
  }
  return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

/** Converts the RGBA bytes of a `tx3g` colour into ARGB. */
uint32_t rgbaToArgb(const uint32_t rgba) {
  const uint32_t red = (rgba >> 24) & 0xFF;
  const uint32_t green = (rgba >> 16) & 0xFF;
  const uint32_t blue = (rgba >> 8) & 0xFF;
  const uint32_t alpha = rgba & 0xFF;
  return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

/** The default style of a `tx3g` track, as its sample description states it. */
struct DefaultStyle final {
  int fontFace = kDefaultFontFace;
  uint32_t textColor = kDefaultTextColor;
  std::string fontFamily;
  float verticalPlacement = kDefaultVerticalPlacement;
  bool customVerticalPlacement = false;
  int calculatedVideoTrackHeight = 0;
  bool windowColorSet = false;
  uint32_t windowColor = 0;
};

/**
 * Parses the sample description of a `tx3g` track.
 *
 * <p>The layout is the one of the 3GPP timed text sample entry, from the `displayFlags` field: the
 * flags, the two justifications, the background colour, the text box and the default style record,
 * followed by the font table that names the fonts of the track.
 */
DefaultStyle parseDefaultStyle(const std::vector<uint8_t>& data) {
  DefaultStyle style;
  const uint8_t* bytes = data.data();
  const size_t size = data.size();
  if (size < 30) {
    return style;
  }
  const uint32_t displayFlags = readUInt32(bytes, size, 0);
  style.fontFace = bytes[24];
  style.textColor = rgbaToArgb(readUInt32(bytes, size, 26));
  style.windowColor = rgbaToArgb(readUInt32(bytes, size, 6));
  // A window that the track draws behind its text is only reported when it is not fully
  // transparent: an alpha of zero states that the text has no window at all.
  style.windowColorSet = (style.windowColor >> 24) != 0;
  const int fontSize = bytes[25];
  style.customVerticalPlacement = (displayFlags & 0x20000000u) != 0;
  if (fontSize > 0) {
    style.calculatedVideoTrackHeight = 20 * fontSize;
  }
  if (style.customVerticalPlacement && style.calculatedVideoTrackHeight > 0) {
    const int requestedVerticalPlacement = readUInt16(bytes, size, 10);
    style.verticalPlacement =
      static_cast<float>(requestedVerticalPlacement) / style.calculatedVideoTrackHeight;
    style.verticalPlacement = std::min(std::max(style.verticalPlacement, 0.0f), 0.95f);
  }
  // The font table follows the default style record: a box with the fonts of the track, each with
  // its identifier and its name. The first font is the one the default style names.
  size_t offset = 30;
  while (offset + 8 <= size) {
    const uint32_t boxSize = readUInt32(bytes, size, offset);
    const uint32_t boxType = readUInt32(bytes, size, offset + 4);
    if (boxSize < 8 || boxSize > size - offset) {
      break;
    }
    if (boxType == kTypeFtab) {
      const size_t entryOffset = offset + 8;
      const uint32_t entryCount = readUInt16(bytes, size, entryOffset);
      if (entryCount > 0 && entryOffset + 5 <= size) {
        const size_t nameLength = bytes[entryOffset + 4];
        const size_t nameOffset = entryOffset + 5;
        if (nameLength > 0 && nameOffset + nameLength <= size) {
          style.fontFamily.assign(reinterpret_cast<const char*>(bytes) + nameOffset, nameLength);
        }
      }
      break;
    }
    offset += boxSize;
  }
  return style;
}

/** Maps a character index of a `tx3g` style record to a byte offset of a UTF-8 string. */
size_t byteOffsetForCharacterIndex(const std::string& text, const uint32_t characterIndex) {
  if (characterIndex == 0 || text.empty()) {
    return 0;
  }
  size_t byteOffset = 0;
  uint32_t units = 0;
  while (byteOffset < text.size() && units < characterIndex) {
    const auto lead = static_cast<uint8_t>(text[byteOffset]);
    size_t length = 1;
    uint32_t codePoint = lead;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      codePoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      codePoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      codePoint = lead & 0x07;
    }
    for (size_t i = 1; i < length && byteOffset + i < text.size(); ++i) {
      codePoint = (codePoint << 6) | (static_cast<uint8_t>(text[byteOffset + i]) & 0x3F);
    }
    byteOffset += std::min(length, text.size() - byteOffset);
    units += codePoint > 0xFFFF ? 2 : 1;
  }
  return byteOffset;
}

/** Attaches the spans of the default style and of the style records of a sample. */
void attachStyle(AVCue& cue, const DefaultStyle& style) {
  if (!cue.text.empty() && style.textColor != 0) {
    AVCueSpan color;
    color.type = AVCueSpan::Type::kForegroundColor;
    color.start = 0;
    color.end = cue.text.size();
    color.color = style.textColor;
    cue.spans.push_back(std::move(color));
  }
  if (!cue.text.empty() &&
      (style.fontFace & (kFontFaceBold | kFontFaceItalic | kFontFaceUnderline)) != 0) {
    AVCueSpan attributes;
    attributes.type = AVCueSpan::Type::kTextStyle;
    attributes.start = 0;
    attributes.end = cue.text.size();
    attributes.style.bold = (style.fontFace & kFontFaceBold) != 0;
    attributes.style.italic = (style.fontFace & kFontFaceItalic) != 0;
    attributes.style.underline = (style.fontFace & kFontFaceUnderline) != 0;
    cue.spans.push_back(std::move(attributes));
  }
  if (!cue.text.empty() && !style.fontFamily.empty()) {
    AVCueSpan family;
    family.type = AVCueSpan::Type::kTextStyle;
    family.start = 0;
    family.end = cue.text.size();
    family.style.fontFamily = style.fontFamily;
    cue.spans.push_back(std::move(family));
  }
}

/** Decodes the text of a sample, honouring the byte order mark if there is one. */
std::string decodeText(const uint8_t* data, const size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
    std::string text;
    text::appendUtf16AsUtf8(text, data + 2, size - 2, false);
    return text;
  }
  if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    std::string text;
    text::appendUtf16AsUtf8(text, data + 2, size - 2, true);
    return text;
  }
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    return std::string(reinterpret_cast<const char*>(data) + 3, size - 3);
  }
  // The specification states the text in UTF-8, and ExoPlayer decodes a text without a byte order
  // mark as UTF-8 as well.
  return std::string(reinterpret_cast<const char*>(data), size);
}

} // namespace

struct AVTx3gParser::Impl final {
  DefaultStyle defaultStyle;
  bool haveInitializationData = false;
};

AVTx3gParser::AVTx3gParser(const std::vector<std::vector<uint8_t>>& initializationData)
  : mImpl(new Impl()) {
  if (!initializationData.empty() && !initializationData[0].empty()) {
    mImpl->defaultStyle = parseDefaultStyle(initializationData[0]);
    mImpl->haveInitializationData = true;
  }
}

AVTx3gParser::~AVTx3gParser() = default;

bool AVTx3gParser::parse(const uint8_t* data, const size_t size, const OutputOptions& options,
                         const CueConsumer& output) {
  if (data == nullptr || output == nullptr) {
    return false;
  }
  AVCuesWithTiming result;
  result.startTimeUs = Definitions::kTimeUnset;
  result.durationUs = Definitions::kTimeUnset;

  if (size < 2) {
    // A sample that cannot hold the length of its text carries no cue, and still replaces the
    // cues of the previous one.
    output(result);
    return true;
  }
  const size_t textLength = readUInt16(data, size, 0);
  if (textLength > size - 2) {
    return false;
  }
  if (textLength == 0) {
    output(result);
    return true;
  }

  AVCue cue;
  cue.text = decodeText(data + 2, textLength);
  if (cue.text.empty()) {
    output(result);
    return true;
  }
  attachStyle(cue, mImpl->defaultStyle);
  cue.windowColorSet = mImpl->defaultStyle.windowColorSet;
  if (cue.windowColorSet) {
    cue.windowColor = mImpl->defaultStyle.windowColor;
  }
  cue.line = mImpl->defaultStyle.verticalPlacement;
  cue.lineType = AVCue::LineType::kFraction;
  cue.lineAnchor = AVCue::AnchorType::kStart;

  // The boxes that follow the text decorate it: `styl` carries the style records of the sample and
  // `tbox` moves the cue within the frame.
  size_t offset = 2 + textLength;
  while (offset + 8 <= size) {
    const uint32_t boxSize = readUInt32(data, size, offset);
    const uint32_t boxType = readUInt32(data, size, offset + 4);
    if (boxSize < 8 || boxSize > size - offset) {
      break;
    }
    const size_t payloadOffset = offset + 8;
    const size_t payloadSize = boxSize - 8;
    if (boxType == kTypeStyl) {
      const uint32_t count = std::min<uint32_t>(readUInt16(data, size, payloadOffset),
                                                kMaxStyleRecordCount);
      size_t recordOffset = payloadOffset + 2;
      for (uint32_t i = 0; i < count; ++i) {
        if (recordOffset + 12 > offset + boxSize) {
          break;
        }
        const uint32_t startChar = readUInt16(data, size, recordOffset);
        const uint32_t endChar = readUInt16(data, size, recordOffset + 2);
        const int fontFace = data[recordOffset + 6];
        const uint32_t color = rgbaToArgb(readUInt32(data, size, recordOffset + 8));
        size_t start = byteOffsetForCharacterIndex(cue.text, startChar);
        size_t end = byteOffsetForCharacterIndex(cue.text, endChar);
        if (end > start) {
          AVCueSpan span;
          span.type = AVCueSpan::Type::kTextStyle;
          span.start = start;
          span.end = end;
          span.style.bold = (fontFace & kFontFaceBold) != 0;
          span.style.italic = (fontFace & kFontFaceItalic) != 0;
          span.style.underline = (fontFace & kFontFaceUnderline) != 0;
          cue.spans.push_back(std::move(span));
          if (color != 0 && color != mImpl->defaultStyle.textColor) {
            AVCueSpan colorSpan;
            colorSpan.type = AVCueSpan::Type::kForegroundColor;
            colorSpan.start = start;
            colorSpan.end = end;
            colorSpan.color = color;
            cue.spans.push_back(std::move(colorSpan));
          }
        }
        recordOffset += 12;
      }
    } else if (boxType == kTypeTbox && mImpl->defaultStyle.customVerticalPlacement &&
               mImpl->defaultStyle.calculatedVideoTrackHeight > 0 && payloadSize >= 2) {
      const int requested = readUInt16(data, size, payloadOffset);
      float placement = static_cast<float>(requested) / mImpl->defaultStyle.calculatedVideoTrackHeight;
      cue.line = std::min(std::max(placement, 0.0f), 0.95f);
    }
    offset += boxSize;
  }

  result.cues.push_back(std::move(cue));
  (void)options;
  output(result);
  return true;
}

void AVTx3gParser::reset() {
  // The parser holds no state across samples.
}

} // namespace coral
