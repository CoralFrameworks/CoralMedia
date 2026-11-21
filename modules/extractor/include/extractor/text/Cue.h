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

#ifndef CORAL_EXTRACTOR_TEXT_CUE_H
#define CORAL_EXTRACTOR_TEXT_CUE_H

#include <media/Types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace coral {

/**
 * One styled range of a {@link AVCue}, or one annotation that the text of the cue carries.
 *
 * <p>This is the counterpart of the Android spans that ExoPlayer attaches to the text of a cue:
 * {@code StyleSpan}, {@code ForegroundColorSpan}, {@code BackgroundColorSpan}, {@code TypefaceSpan},
 * {@code AbsoluteSizeSpan}, {@code RelativeSizeSpan}, {@code UnderlineSpan},
 * {@code StrikethroughSpan}, {@code RubySpan}, {@code VoiceSpan}, {@code LanguageFeatureSpan},
 * {@code TextEmphasisSpan} and {@code HorizontalTextInVerticalContextSpan}. A C++ consumer draws the
 * text itself, so what it needs is the attribute rather than the span object of a platform: the
 * attributes are collapsed into this value type, and the meaning of a span follows its
 * {@link Type}.
 *
 * <p>The range is a pair of byte offsets into {@link AVCue::text}, end exclusive. Byte offsets rather
 * than code point indices keep the span usable together with the UTF-8 string it decorates, which is
 * what a renderer measures and draws.
 */
class AVCueSpan final {
public:
  /** What a span decorates. */
  enum class Type : uint8_t {
    /** Font attributes ({@link #style}). */
    kTextStyle = 0,
    /** The colour of the text ({@link #color}). */
    kForegroundColor = 1,
    /** The colour of the box drawn behind the text ({@link #color}). */
    kBackgroundColor = 2,
    /** Ruby annotation ({@link #ruby}). */
    kRuby = 3,
    /** The speaker of the text ({@link #text}). */
    kVoice = 4,
    /** The language of the text ({@link #text}), as an IETF BCP 47 tag. */
    kLanguage = 5,
    /** Emphasis marks ({@link #emphasis}). */
    kTextEmphasis = 6,
    /** Text that stays horizontal inside a vertical layout. */
    kHorizontalTextInVerticalContext = 7,
    /** Text that is lowered below the baseline, usually drawn smaller (SubRip `<sub>`). */
    kSubscript = 8,
    /** Text that is raised above the baseline, usually drawn smaller (SubRip `<sup>`). */
    kSuperscript = 9,
  };

  /** An unset text size, in {@link #TextStyle}. */
  static constexpr float kTextSizeUnset = -std::numeric_limits<float>::max();

  /** How a ruby annotation is laid out against the base text. */
  enum class RubyType : uint8_t {
    /** The span covers the annotation text. */
    kText = 0,
    /** The span covers the annotated base text. */
    kBase = 1,
  };

  /** Where a ruby annotation goes, relative to the base text. */
  enum class RubyPosition : uint8_t {
    //!< The position is unknown.
    kUnknown = 0,
    //!< Above horizontal text, or right of vertical text.
    kBefore = 1,
    //!< Below horizontal text, or left of vertical text.
    kAfter = 2,
  };

  /** The shape of a text emphasis mark. */
  enum class TextEmphasisMark : uint8_t {
    //!< The shape is unset.
    kNone = 0,
    //!< A dot.
    kDot = 1,
    //!< A circle.
    kCircle = 2,
    //!< A double circle.
    kDoubleCircle = 3,
    //!< A triangle.
    kTriangle = 4,
    //!< A sesame seed.
    kSesame = 5,
  };

  /** Where a text emphasis mark goes. */
  enum class TextEmphasisMode : uint8_t {
    //!< The mode is unknown.
    kUnknown = 0,
    //!< After the text.
    kAfter = 1,
    //!< Before the text.
    kBefore = 2,
  };

  /** The font attributes of a {@link Type::kTextStyle} span. */
  struct TextStyle final {
    //!< Whether the text is bold.
    bool bold = false;
    //!< Whether the text is italic.
    bool italic = false;
    //!< Whether the text is underlined.
    bool underline = false;
    //!< Whether the text is struck out.
    bool strikeout = false;
    //!< The requested font family, or empty to keep the family of the cue.
    std::string fontFamily;
    //!< A scale of the cue text size, an absolute size in pixels when {@link #textSizeAbsolute} is
    //!< set, or {@link #kTextSizeUnset} to keep the size of the cue.
    float textSize = kTextSizeUnset;
    //!< Whether {@link #textSize} is a pixel size rather than a scale.
    bool textSizeAbsolute = false;
  };

  /** The annotation of a {@link Type::kRuby} span. */
  struct Ruby final {
    //!< The annotation text.
    std::string text;
    //!< Whether the span is the annotation or the annotated text.
    RubyType type = RubyType::kText;
    //!< Where the annotation goes.
    RubyPosition position = RubyPosition::kUnknown;
  };

  /** The mark of a {@link Type::kTextEmphasis} span. */
  struct Emphasis final {
    //!< The shape of the mark.
    TextEmphasisMark mark = TextEmphasisMark::kNone;
    //!< Where the mark goes.
    TextEmphasisMode mode = TextEmphasisMode::kUnknown;
    //!< The colour of the mark, as ARGB, or 0 to use the text colour.
    uint32_t fillColor = 0;
  };

  /** What this span decorates. */
  Type type = Type::kTextStyle;

  /** The first byte of {@link AVCue::text} the span covers. */
  size_t start = 0;

  /** The offset of the end of the span, exclusive. */
  size_t end = 0;

  /** The colour of a colour span, as ARGB. */
  uint32_t color = 0;

  /** The attributes of a text style span. */
  TextStyle style;

  /** The annotation of a ruby span. */
  Ruby ruby;

  /** The mark of a text emphasis span. */
  Emphasis emphasis;

  /** The voice of a voice span, or the language tag of a language span. */
  std::string text;

  /** Returns whether the span is empty, i.e. whether it covers no text. */
  [[nodiscard]] bool isEmpty() const { return end <= start; }
};

/** Compares the two spans by value. */
bool operator==(const AVCueSpan& lhs, const AVCueSpan& rhs);

/** Compares the two spans by value. */
inline bool operator!=(const AVCueSpan& lhs, const AVCueSpan& rhs) {
  return !(lhs == rhs);
}

/**
 * The pixels of an image cue, i.e. of a subtitle format that stores a picture rather than text.
 *
 * <p>An image cue is a picture per cue, so the pixels are owned by the cue: a renderer uploads them
 * once when the cue becomes active and never has to keep the sample of the container alive.
 */
struct AVCueBitmap final {
  /** The width of the picture in pixels. */
  int width = 0;

  /** The height of the picture in pixels. */
  int height = 0;

  /** The layout of {@link #pixels}: {@link PixelFormat::kFormatRGBA} or
   * {@link PixelFormat::kFormatBGRA}. */
  PixelFormat format = PixelFormat::kFormatRGBA;

  /** The distance between the starts of two rows; 0 means tightly packed. */
  uint32_t rowBytes = 0;

  /** Whether the colour channels of {@link #pixels} were multiplied by the alpha channel. */
  bool premultiplied = true;

  /** The pixel data, {@link #rowBytes} times {@link #height} bytes. */
  std::vector<uint8_t> pixels;

  /**
   * The encoded bytes of the picture (PNG, JPEG or WebP), when the parser did not decode it.
   *
   * <p>A renderer that understands the format decodes {@link #pixels} when it is present and falls
   * back to this otherwise; TTML carries its images base64 encoded, and the extractor does not link
   * an image codec.
   */
  std::vector<uint8_t> encoded;

  /** Returns whether the bitmap holds no picture. */
  [[nodiscard]] bool isEmpty() const {
    return (width <= 0 || height <= 0 || pixels.empty()) && encoded.empty();
  }
};

/** Compares the two bitmaps by value. */
bool operator==(const AVCueBitmap& lhs, const AVCueBitmap& rhs);

/** Compares the two bitmaps by value. */
inline bool operator!=(const AVCueBitmap& lhs, const AVCueBitmap& rhs) {
  return !(lhs == rhs);
}

/**
 * A subtitle to show at a time, as text with spans or as a picture.
 *
 * <p>This is the counterpart of ExoPlayer's {@code androidx.media3.common.text.Cue}. The fields that
 * a renderer interprets are described in the javadoc of the Java class and are kept here with the
 * same names, so that a format parser can be reviewed against the Java implementation it was ported
 * from.
 *
 * <p>A cue is a plain value: it can be copied, compared and stored in a cue group. A cue whose
 * {@link #bitmap} is empty is a text cue, and a cue whose {@link #text} is empty is an image cue;
 * one of the two is always set for a cue that came from a parser.
 */
class AVCue final {
public:
  /** An unset dimension, wherever a fraction or a size is expected. */
  static constexpr float kDimenUnset = -std::numeric_limits<float>::max();

  /** The default window colour of a cue, an opaque black. */
  static constexpr uint32_t kDefaultWindowColor = 0xFF000000u;

  /** The anchor of a cue box. */
  enum class AnchorType : uint8_t {
    //!< The anchor is unset.
    kUnset = 0,
    //!< The start edge of the box is anchored.
    kStart = 1,
    //!< The middle of the box is anchored.
    kMiddle = 2,
    //!< The end edge of the box is anchored.
    kEnd = 3,
  };

  /** The unit of {@link #line}. */
  enum class LineType : uint8_t {
    //!< The unit is unset.
    kUnset = 0,
    //!< The line is a fraction of the viewport.
    kFraction = 1,
    //!< The line is a viewport line number.
    kNumber = 2,
  };

  /** The unit of {@link #textSize}. */
  enum class TextSizeType : uint8_t {
    //!< The unit is unset.
    kUnset = 0,
    //!< A fraction of the viewport size minus the padding.
    kFractional = 1,
    //!< A fraction of the viewport size, ignoring the padding.
    kFractionalIgnorePadding = 2,
    //!< An absolute size in pixels.
    kAbsolute = 3,
  };

  /** The unit of {@link #position}. */
  enum class PositionType : uint8_t {
    //!< The unit is unset and the position is a viewport fraction.
    kUnset = 0,
    //!< A fraction of the viewport width.
    kFraction = 1,
    //!< An absolute pixel offset.
    kAbsolute = 2,
  };

  /** The writing direction of a cue. */
  enum class VerticalType : uint8_t {
    //!< Horizontal text.
    kUnset = 0,
    //!< Vertical text, read from the right to the left.
    kRightToLeft = 1,
    //!< Vertical text, read from the left to the right.
    kLeftToRight = 2,
  };

  /** The alignment of the lines of a cue, or of the cue box within the viewport. */
  enum class Alignment : uint8_t {
    //!< The alignment is unset.
    kUnset = 0,
    //!< Left.
    kLeft = 1,
    //!< Centred.
    kCenter = 2,
    //!< Right.
    kRight = 3,
    //!< The start edge of the writing direction.
    kStart = 4,
    //!< The end edge of the writing direction.
    kEnd = 5,
  };

  /** The text of the cue, or empty for an image cue. */
  std::string text;

  /** The styled ranges of {@link #text}, ordered by their start offset. */
  std::vector<AVCueSpan> spans;

  /** The picture of the cue, or an empty bitmap for a text cue. */
  AVCueBitmap bitmap;

  /** The alignment of the text within the cue box. */
  Alignment textAlignment = Alignment::kUnset;

  /** The alignment of the lines of the cue relative to the longest line. */
  Alignment multiRowAlignment = Alignment::kUnset;

  /** The position of the cue box in the direction orthogonal to the writing direction, or
   * {@link #kDimenUnset}. */
  float line = kDimenUnset;

  /** The unit of {@link #line}. */
  LineType lineType = LineType::kUnset;

  /** The anchor of the cue box that {@link #line} positions. */
  AnchorType lineAnchor = AnchorType::kUnset;

  /** The position of the cue box in the direction of the writing direction, or
   * {@link #kDimenUnset}. */
  float position = kDimenUnset;

  /** The unit of {@link #position}; unset means a viewport fraction. */
  PositionType positionType = PositionType::kUnset;

  /** The anchor of the cue box that {@link #position} positions. */
  AnchorType positionAnchor = AnchorType::kUnset;

  /** The width of the cue box, as a fraction of the viewport, or {@link #kDimenUnset}. */
  float size = kDimenUnset;

  /** The additional left inset of the cue text, as a fraction of the viewport width. */
  float paragraphIndent = 0.0f;

  /** The height of an image cue, as a fraction of the viewport, or {@link #kDimenUnset}. */
  float bitmapHeight = kDimenUnset;

  /** Whether {@link #windowColor} describes a window that must be drawn. */
  bool windowColorSet = false;

  /** The colour of the window behind the cue, as ARGB. */
  uint32_t windowColor = kDefaultWindowColor;

  /** The default size of the cue text. */
  float textSize = kDimenUnset;

  /** The unit of {@link #textSize}. */
  TextSizeType textSizeType = TextSizeType::kUnset;

  /** The writing direction of the cue. */
  VerticalType verticalType = VerticalType::kUnset;

  /** The shear angle of the cue in degrees, in graphics coordinates. */
  float shearDegrees = 0.0f;

  /** The z index of the cue; a larger index is drawn above a smaller one. */
  int zIndex = 0;

  /** The maximum visible lines of a WebVTT region, or 0 when the cue has no line limit. */
  int regionLines = 0;

  /** Returns whether the cue holds no text and no picture. */
  [[nodiscard]] bool isEmpty() const { return text.empty() && bitmap.isEmpty(); }

  /** Returns whether the cue is an image cue. */
  [[nodiscard]] bool isBitmap() const { return !bitmap.isEmpty(); }

  /**
   * Returns whether the cue asks for a rendering attribute rather than only for text.
   *
   * <p>Mirrors ExoPlayer's {@code Cue.hasRenderingAttributes}: a player that lays out the cue itself
   * has to consult these fields, whereas a cue without them is drawn with the default style.
   */
  [[nodiscard]] bool hasRenderingAttributes() const {
    return textAlignment != Alignment::kUnset || multiRowAlignment != Alignment::kUnset ||
           line != kDimenUnset || lineType != LineType::kUnset || lineAnchor != AnchorType::kUnset ||
           position != kDimenUnset || positionAnchor != AnchorType::kUnset || size != kDimenUnset ||
           bitmapHeight != kDimenUnset || textSizeType != TextSizeType::kUnset ||
           textSize != kDimenUnset || verticalType != VerticalType::kUnset || shearDegrees != 0.0f ||
           paragraphIndent != 0.0f || regionLines > 0 || positionType != PositionType::kUnset;
  }

  /** Returns a human readable description of the cue, for diagnostics. */
  [[nodiscard]] std::string toString() const;
};

/** Compares the two cues by value. */
bool operator==(const AVCue& lhs, const AVCue& rhs);

/** Compares the two cues by value. */
inline bool operator!=(const AVCue& lhs, const AVCue& rhs) {
  return !(lhs == rhs);
}

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CUE_H
