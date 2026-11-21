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

#include <extractor/text/Cue.h>

#include <sstream>
#include <string>

namespace coral {

bool operator==(const AVCueSpan& lhs, const AVCueSpan& rhs) {
  if (lhs.type != rhs.type || lhs.start != rhs.start || lhs.end != rhs.end ||
      lhs.color != rhs.color || lhs.text != rhs.text) {
    return false;
  }
  const auto& leftStyle = lhs.style;
  const auto& rightStyle = rhs.style;
  if (leftStyle.bold != rightStyle.bold || leftStyle.italic != rightStyle.italic ||
      leftStyle.underline != rightStyle.underline || leftStyle.strikeout != rightStyle.strikeout ||
      leftStyle.fontFamily != rightStyle.fontFamily ||
      leftStyle.textSizeAbsolute != rightStyle.textSizeAbsolute ||
      leftStyle.textSize != rightStyle.textSize) {
    return false;
  }
  const auto& leftRuby = lhs.ruby;
  const auto& rightRuby = rhs.ruby;
  if (leftRuby.text != rightRuby.text || leftRuby.type != rightRuby.type ||
      leftRuby.position != rightRuby.position) {
    return false;
  }
  const auto& leftEmphasis = lhs.emphasis;
  const auto& rightEmphasis = rhs.emphasis;
  return leftEmphasis.mark == rightEmphasis.mark && leftEmphasis.mode == rightEmphasis.mode &&
         leftEmphasis.fillColor == rightEmphasis.fillColor;
}

bool operator==(const AVCueBitmap& lhs, const AVCueBitmap& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.format == rhs.format &&
         lhs.rowBytes == rhs.rowBytes && lhs.premultiplied == rhs.premultiplied &&
         lhs.pixels == rhs.pixels && lhs.encoded == rhs.encoded;
}

bool operator==(const AVCue& lhs, const AVCue& rhs) {
  return lhs.text == rhs.text && lhs.spans == rhs.spans && lhs.bitmap == rhs.bitmap &&
         lhs.textAlignment == rhs.textAlignment &&
         lhs.multiRowAlignment == rhs.multiRowAlignment && lhs.line == rhs.line &&
         lhs.lineType == rhs.lineType && lhs.lineAnchor == rhs.lineAnchor &&
         lhs.position == rhs.position && lhs.positionAnchor == rhs.positionAnchor &&
         lhs.positionType == rhs.positionType && lhs.size == rhs.size &&
         lhs.bitmapHeight == rhs.bitmapHeight &&
         lhs.paragraphIndent == rhs.paragraphIndent &&
         lhs.windowColorSet == rhs.windowColorSet && lhs.windowColor == rhs.windowColor &&
         lhs.textSize == rhs.textSize && lhs.textSizeType == rhs.textSizeType &&
         lhs.verticalType == rhs.verticalType && lhs.shearDegrees == rhs.shearDegrees &&
         lhs.zIndex == rhs.zIndex && lhs.regionLines == rhs.regionLines;
}

std::string AVCue::toString() const {
  std::ostringstream stream;
  stream << "AVCue{";
  if (!text.empty()) {
    stream << "text=\"" << text << "\"";
  } else {
    stream << "bitmap=" << bitmap.width << "x" << bitmap.height;
  }
  if (!spans.empty()) {
    stream << ",spans=" << spans.size();
  }
  if (hasRenderingAttributes()) {
    stream << ",renderingAttributes";
  }
  if (zIndex != 0) {
    stream << ",zIndex=" << zIndex;
  }
  stream << "}";
  return stream.str();
}

} // namespace coral
