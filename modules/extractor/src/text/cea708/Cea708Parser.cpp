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

#include <extractor/text/cea708/Cea708Parser.h>

#include "text/SubtitleTextUtil.h"

#include <media/Definitions.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr int kNumWindows = 8;
constexpr int kMaximumRowCount = 15;

constexpr uint8_t kDtvccPacketData = 0x02;
constexpr uint8_t kDtvccPacketStart = 0x03;
constexpr uint8_t kCcValidFlag = 0x04;

constexpr int kGroupC0End = 0x1F;
constexpr int kGroupG0End = 0x7F;
constexpr int kGroupC1End = 0x9F;
constexpr int kGroupG1End = 0xFF;

constexpr int kCommandNul = 0x00;
constexpr int kCommandEtx = 0x03;
constexpr int kCommandBs = 0x08;
constexpr int kCommandFf = 0x0C;
constexpr int kCommandCr = 0x0D;
constexpr int kCommandHcr = 0x0E;
constexpr int kCommandExt1 = 0x10;
constexpr int kCommandExt1Start = 0x11;
constexpr int kCommandExt1End = 0x17;
constexpr int kCommandP16Start = 0x18;
constexpr int kCommandP16End = 0x1F;

constexpr int kCommandCw0 = 0x80;
constexpr int kCommandClw = 0x88;
constexpr int kCommandDsw = 0x89;
constexpr int kCommandHdw = 0x8A;
constexpr int kCommandTgw = 0x8B;
constexpr int kCommandDlw = 0x8C;
constexpr int kCommandDly = 0x8D;
constexpr int kCommandDlc = 0x8E;
constexpr int kCommandRst = 0x8F;
constexpr int kCommandSpa = 0x90;
constexpr int kCommandSpc = 0x91;
constexpr int kCommandSpl = 0x92;
constexpr int kCommandSwa = 0x97;
constexpr int kCommandDf0 = 0x98;

constexpr uint32_t kColorWhite = 0xFFFFFFFFu;
constexpr uint32_t kColorBlack = 0xFF000000u;
constexpr uint32_t kColorTransparent = 0x00000000u;

constexpr int kJustificationLeft = 0;
constexpr int kJustificationRight = 1;
constexpr int kJustificationCenter = 2;
constexpr int kJustificationFull = 3;

/** The subset of the caption window style table that affects the cue box. */
constexpr uint32_t kWindowStyleFill[] = {
  kColorBlack, kColorTransparent, kColorBlack, kColorBlack,
  kColorTransparent, kColorBlack, kColorBlack,
};
constexpr int kWindowStyleJustification[] = {
  kJustificationLeft, kJustificationLeft, kJustificationLeft, kJustificationLeft,
  kJustificationLeft, kJustificationCenter, kJustificationLeft,
};
constexpr uint32_t kPenStyleBackground[] = {
  kColorBlack, kColorBlack, kColorBlack, kColorBlack,
  kColorBlack, kColorTransparent, kColorTransparent,
};

/** Encodes one Unicode code point as UTF-8. */
std::string utf8(const uint32_t codePoint) {
  std::string result;
  if (codePoint < 0x80) {
    result.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x10000) {
    result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
  return result;
}

uint32_t ceaColor(const int red, const int green, const int blue, const int opacity = 0) {
  const uint32_t alpha = opacity <= 1 ? 255u : opacity == 2 ? 127u : 0u;
  return (alpha << 24) | (red > 1 ? 0x00FF0000u : 0u) | (green > 1 ? 0x0000FF00u : 0u) |
         (blue > 1 ? 0x000000FFu : 0u);
}

/** An MSB-first bit reader over the bytes of a DTVCC packet. */
class BitReader final {
public:
  BitReader(const uint8_t* data, const size_t size) : data_(data), size_(size) {
  }

  int readBits(const int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
      value = (value << 1) | readBit();
    }
    return value;
  }

  int readBit() {
    if (position_ >= size_ * 8) {
      return 0;
    }
    const int value = (data_[position_ / 8] >> (7 - (position_ % 8))) & 1;
    ++position_;
    return value;
  }

  void skipBits(const size_t count) {
    position_ = std::min(position_ + count, size_ * 8);
  }

  [[nodiscard]] size_t position() const {
    return position_;
  }

  [[nodiscard]] size_t bitsLeft() const {
    return size_ * 8 - position_;
  }

private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
};

/** One character with the pen attributes that were active when it was written. */
struct StyledChar final {
  std::string text;
  bool italic = false;
  bool underline = false;
  uint32_t foreground = kColorWhite;
  uint32_t background = kColorBlack;
};

using Line = std::vector<StyledChar>;

/** One CEA-708 caption window and its pen state. */
class CaptionWindow final {
public:
  [[nodiscard]] bool isEmpty() const {
    return !defined_ || (lines_.empty() && current_.empty());
  }

  [[nodiscard]] bool isDefined() const {
    return defined_;
  }

  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

  void setVisible(const bool visible) {
    visible_ = visible;
  }

  [[nodiscard]] bool isDefinedAndVisible() const {
    return defined_ && visible_;
  }

  void reset() {
    clear();
    defined_ = false;
    visible_ = false;
    priority_ = 4;
    relativePositioning_ = false;
    verticalAnchor_ = 0;
    horizontalAnchor_ = 0;
    anchorId_ = 0;
    rowCount_ = kMaximumRowCount;
    justification_ = kJustificationLeft;
    windowStyleId_ = 0;
    penStyleId_ = 0;
    fillColor_ = kColorBlack;
    forecast_ = kColorWhite;
    backgroundColor_ = kColorBlack;
    italic_ = false;
    underline_ = false;
  }

  void clear() {
    lines_.clear();
    current_.clear();
    row_ = 0;
  }

  void defineWindow(const bool visible, const int priority, const bool relativePositioning,
      const int verticalAnchor, const int horizontalAnchor, const int rowCount, const int anchorId,
      const int windowStyleId, const int penStyleId) {
    defined_ = true;
    visible_ = visible;
    priority_ = priority;
    relativePositioning_ = relativePositioning;
    verticalAnchor_ = verticalAnchor;
    horizontalAnchor_ = horizontalAnchor;
    anchorId_ = anchorId;
    rowCount_ = std::clamp(rowCount + 1, 1, kMaximumRowCount);
    while (static_cast<int>(lines_.size()) >= rowCount_ ||
           static_cast<int>(lines_.size()) >= kMaximumRowCount) {
      lines_.erase(lines_.begin());
    }
    if (windowStyleId > 0 && windowStyleId <= 7 && windowStyleId_ != windowStyleId) {
      windowStyleId_ = windowStyleId;
      fillColor_ = kWindowStyleFill[windowStyleId - 1];
      justification_ = kWindowStyleJustification[windowStyleId - 1];
    }
    if (penStyleId > 0 && penStyleId <= 7 && penStyleId_ != penStyleId) {
      penStyleId_ = penStyleId;
      forecast_ = kColorWhite;
      backgroundColor_ = kPenStyleBackground[penStyleId - 1];
    }
  }

  void setWindowAttributes(const uint32_t fillColor, const int justification) {
    fillColor_ = fillColor;
    justification_ = justification;
  }

  void setPenAttributes(const bool italic, const bool underline) {
    italic_ = italic;
    underline_ = underline;
  }

  void setPenColor(const uint32_t foreground, const uint32_t background) {
    forecast_ = foreground;
    backgroundColor_ = background;
  }

  void setPenLocation(const int row) {
    if (row_ != row) {
      appendNewline();
    }
    row_ = row;
  }

  void backspace() {
    if (!current_.empty()) {
      current_.pop_back();
    }
  }

  void append(const uint32_t codePoint) {
    if (codePoint == '\n') {
      appendNewline();
      return;
    }
    StyledChar character;
    character.text = utf8(codePoint);
    character.italic = italic_;
    character.underline = underline_;
    character.foreground = forecast_;
    character.background = backgroundColor_;
    current_.push_back(std::move(character));
  }

  /** Builds the cue for this window, or returns false when the window has no text. */
  bool build(AVCue& cue, int& priority) const {
    if (isEmpty()) {
      return false;
    }
    std::vector<Line> lines = lines_;
    Line current = current_;
    trimTrailingSpaces(current);
    if (!current.empty()) {
      lines.push_back(std::move(current));
    }
    std::vector<StyledChar> characters;
    for (size_t i = 0; i < lines.size(); ++i) {
      for (const auto& character: lines[i]) {
        characters.push_back(character);
      }
      if (i + 1 < lines.size()) {
        StyledChar newline;
        newline.text = "\n";
        characters.push_back(std::move(newline));
      }
    }
    if (characters.empty()) {
      return false;
    }

    std::string text;
    std::vector<size_t> offsets;
    offsets.reserve(characters.size());
    for (const auto& character: characters) {
      offsets.push_back(text.size());
      text += character.text;
    }
    offsets.push_back(text.size());
    if (text.empty()) {
      return false;
    }
    cue.text = std::move(text);
    cue.textAlignment = justification_ == kJustificationCenter ? AVCue::Alignment::kCenter
                        : justification_ == kJustificationRight ? AVCue::Alignment::kRight
                                                                : AVCue::Alignment::kLeft;

    appendRuns(cue.spans, characters, offsets);

    const float position = (relativePositioning_ ? verticalPosition(horizontalAnchor_, 99.0f)
                                                 : horizontalPosition(horizontalAnchor_, 209.0f));
    const float line = relativePositioning_ ? horizontalPosition(verticalAnchor_, 99.0f)
                                            : verticalPosition(verticalAnchor_, 74.0f);
    cue.position = position;
    cue.line = line;
    cue.lineType = AVCue::LineType::kFraction;
    cue.positionAnchor = anchorId_ % 3 == 0 ? AVCue::AnchorType::kStart
                        : anchorId_ % 3 == 1 ? AVCue::AnchorType::kMiddle
                                             : AVCue::AnchorType::kEnd;
    cue.lineAnchor = anchorId_ / 3 == 0 ? AVCue::AnchorType::kStart
                    : anchorId_ / 3 == 1 ? AVCue::AnchorType::kMiddle
                                         : AVCue::AnchorType::kEnd;
    cue.windowColorSet = fillColor_ != kColorBlack;
    cue.windowColor = fillColor_;
    priority = priority_;
    return true;
  }

  static uint32_t argbFromCeaColor(const int red, const int green, const int blue,
      const int opacity = 0) {
    return ceaColor(red, green, blue, opacity);
  }

private:
  static float horizontalPosition(const int anchor, const float size) {
    return std::clamp(static_cast<float>(anchor) / size, 0.0f, 1.0f) * 0.9f + 0.05f;
  }

  static float verticalPosition(const int anchor, const float size) {
    return std::clamp(static_cast<float>(anchor) / size, 0.0f, 1.0f) * 0.9f + 0.05f;
  }

  static void trimTrailingSpaces(Line& line) {
    while (!line.empty() && (line.back().text == " " || line.back().text == "\n")) {
      line.pop_back();
    }
  }

  void appendNewline() {
    trimTrailingSpaces(current_);
    lines_.push_back(std::move(current_));
    current_.clear();
    while (static_cast<int>(lines_.size()) >= rowCount_ ||
           static_cast<int>(lines_.size()) >= kMaximumRowCount) {
      lines_.erase(lines_.begin());
    }
    row_ = static_cast<int>(lines_.size());
  }

  static void appendRuns(std::vector<AVCueSpan>& spans,
      const std::vector<StyledChar>& characters, const std::vector<size_t>& offsets) {
    size_t position = 0;
    while (position < characters.size()) {
      const bool italic = characters[position].italic;
      const bool underline = characters[position].underline;
      size_t end = position + 1;
      while (end < characters.size() && characters[end].italic == italic &&
             characters[end].underline == underline) {
        ++end;
      }
      if (italic || underline) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kTextStyle;
        span.start = offsets[position];
        span.end = offsets[end];
        span.style.italic = italic;
        span.style.underline = underline;
        spans.push_back(std::move(span));
      }
      position = end;
    }

    position = 0;
    while (position < characters.size()) {
      const uint32_t color = characters[position].foreground;
      size_t end = position + 1;
      while (end < characters.size() && characters[end].foreground == color) {
        ++end;
      }
      if (color != kColorWhite) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kForegroundColor;
        span.start = offsets[position];
        span.end = offsets[end];
        span.color = color;
        spans.push_back(std::move(span));
      }
      position = end;
    }

    position = 0;
    while (position < characters.size()) {
      const uint32_t color = characters[position].background;
      size_t end = position + 1;
      while (end < characters.size() && characters[end].background == color) {
        ++end;
      }
      if (color != kColorBlack) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kBackgroundColor;
        span.start = offsets[position];
        span.end = offsets[end];
        span.color = color;
        spans.push_back(std::move(span));
      }
      position = end;
    }
  }

  bool defined_ = false;
  bool visible_ = false;
  int priority_ = 4;
  bool relativePositioning_ = false;
  int verticalAnchor_ = 0;
  int horizontalAnchor_ = 0;
  int anchorId_ = 0;
  int rowCount_ = kMaximumRowCount;
  int justification_ = kJustificationLeft;
  int windowStyleId_ = 0;
  int penStyleId_ = 0;
  uint32_t fillColor_ = kColorBlack;

  bool italic_ = false;
  bool underline_ = false;
  uint32_t forecast_ = kColorWhite;
  uint32_t backgroundColor_ = kColorBlack;
  int row_ = 0;
  std::vector<Line> lines_;
  Line current_;
};

} // namespace

class AVCea708Parser::Impl final {
public:
  void feed(const uint8_t* data, const size_t size, std::vector<AVCuesWithTiming>& results) {
    for (size_t position = 0; position + 3 <= size; position += 3) {
      const uint8_t header = data[position];
      const bool valid = (header & kCcValidFlag) != 0;
      const int type = header & 0x03;
      if (!valid || (type != kDtvccPacketData && type != kDtvccPacketStart)) {
        continue;
      }
      const uint8_t first = data[position + 1];
      const uint8_t second = data[position + 2];
      if (type == kDtvccPacketStart) {
        finalizePacket(results);
        sequence_ = (first >> 6) & 0x03;
        if (previousSequence_ != -1 && sequence_ != (previousSequence_ + 1) % 4) {
          resetWindows();
        }
        previousSequence_ = sequence_;
        packetSize_ = first & 0x3F;
        if (packetSize_ == 0) {
          packetSize_ = 64;
        }
        packet_.clear();
        packet_.push_back(second);
        packetActive_ = true;
      } else if (packetActive_) {
        packet_.push_back(first);
        packet_.push_back(second);
      } else {
        continue;
      }
      if (packetActive_ && packet_.size() == static_cast<size_t>(packetSize_ * 2 - 1)) {
        finalizePacket(results);
      }
    }
  }

  void reset() {
    resetWindows();
    packet_.clear();
    packetActive_ = false;
    packetSize_ = 0;
    sequence_ = 0;
    previousSequence_ = -1;
    delayUs_ = 0;
  }

  void setService(const int serviceNumber) {
    if (serviceNumber >= 1 && serviceNumber <= 63) {
      selectedService_ = serviceNumber;
    }
  }

private:
  void finalizePacket(std::vector<AVCuesWithTiming>& results) {
    if (!packetActive_) {
      return;
    }
    processPacket(results);
    packetActive_ = false;
    packet_.clear();
  }

  void processPacket(std::vector<AVCuesWithTiming>& results) {
    BitReader reader(packet_.data(), packet_.size());
    bool cuesNeedUpdate = false;
    while (reader.bitsLeft() >= 8) {
      int serviceNumber = reader.readBits(3);
      const int blockSize = reader.readBits(5);
      if (serviceNumber == 7) {
        reader.skipBits(2);
        serviceNumber = reader.readBits(6);
      }
      if (blockSize == 0) {
        break;
      }
      if (serviceNumber != selectedService_) {
        reader.skipBits(static_cast<size_t>(blockSize) * 8);
        continue;
      }
      const size_t endPosition = reader.position() + static_cast<size_t>(blockSize) * 8;
      while (reader.position() < endPosition && reader.bitsLeft() >= 8) {
        const int command = reader.readBits(8);
        if (command != kCommandExt1) {
          if (command <= kGroupC0End) {
            handleC0(command, reader, results, cuesNeedUpdate);
          } else if (command <= kGroupG0End) {
            handleG0(command);
            cuesNeedUpdate = true;
          } else if (command <= kGroupC1End) {
            handleC1(command, reader, results, cuesNeedUpdate);
          } else if (command <= kGroupG1End) {
            handleG1(command);
            cuesNeedUpdate = true;
          }
        } else {
          const int extended = reader.readBits(8);
          if (extended <= 0x1F) {
            handleC2(extended, reader);
          } else if (extended <= 0x7F) {
            handleG2(extended);
            cuesNeedUpdate = true;
          } else if (extended <= 0x9F) {
            handleC3(extended, reader);
          } else {
            handleG3(extended);
            cuesNeedUpdate = true;
          }
        }
      }
    }
    if (cuesNeedUpdate) {
      emitCues(results);
    }
  }

  void handleC0(const int command, BitReader& reader, std::vector<AVCuesWithTiming>& results,
      bool& cuesNeedUpdate) {
    switch (command) {
      case kCommandNul:
        break;
      case kCommandEtx:
        cuesNeedUpdate = true;
        break;
      case kCommandBs:
        currentWindow().backspace();
        break;
      case kCommandFf:
        resetWindows();
        break;
      case kCommandCr:
        currentWindow().append('\n');
        break;
      case kCommandHcr:
        break;
      default:
        if (command >= kCommandExt1Start && command <= kCommandExt1End) {
          reader.skipBits(8);
        } else if (command >= kCommandP16Start && command <= kCommandP16End) {
          reader.skipBits(16);
        }
        break;
    }
    static_cast<void>(results);
  }

  void handleC1(const int command, BitReader& reader, std::vector<AVCuesWithTiming>& results,
      bool& cuesNeedUpdate) {
    if (command >= kCommandCw0 && command <= kCommandCw0 + 7) {
      currentWindowIndex_ = command - kCommandCw0;
      return;
    }
    switch (command) {
      case kCommandClw:
        for (int i = 0; i < kNumWindows; ++i) {
          if (reader.readBit() != 0) {
            windows_[kNumWindows - 1 - i].clear();
          }
        }
        cuesNeedUpdate = true;
        break;
      case kCommandDsw:
        for (int i = 0; i < kNumWindows; ++i) {
          if (reader.readBit() != 0) {
            windows_[kNumWindows - 1 - i].setVisible(true);
          }
        }
        cuesNeedUpdate = true;
        break;
      case kCommandHdw:
        for (int i = 0; i < kNumWindows; ++i) {
          if (reader.readBit() != 0) {
            windows_[kNumWindows - 1 - i].setVisible(false);
          }
        }
        cuesNeedUpdate = true;
        break;
      case kCommandTgw:
        for (int i = 0; i < kNumWindows; ++i) {
          if (reader.readBit() != 0) {
            const int index = kNumWindows - 1 - i;
            windows_[index].setVisible(!windows_[index].isVisible());
          }
        }
        cuesNeedUpdate = true;
        break;
      case kCommandDlw:
        for (int i = 0; i < kNumWindows; ++i) {
          if (reader.readBit() != 0) {
            windows_[kNumWindows - 1 - i].reset();
          }
        }
        cuesNeedUpdate = true;
        break;
      case kCommandDly: {
        // DLY carries tenths of a second. The cue times of the result are relative to the sample,
        // which is exactly where the delay belongs.
        delayUs_ = static_cast<int64_t>(reader.readBits(8)) * 100000;
        break;
      }
      case kCommandDlc:
        // DelayCancel drops a delay that has not been applied yet.
        delayUs_ = 0;
        break;
      case kCommandRst:
        resetWindows();
        cuesNeedUpdate = true;
        break;
      case kCommandSpa:
        if (!currentWindow().isDefined()) {
          reader.skipBits(16);
        } else {
          reader.readBits(4);
          reader.readBits(2);
          reader.readBits(2);
          const bool italic = reader.readBit() != 0;
          const bool underline = reader.readBit() != 0;
          reader.readBits(3);
          reader.readBits(3);
          currentWindow().setPenAttributes(italic, underline);
        }
        break;
      case kCommandSpc:
        if (!currentWindow().isDefined()) {
          reader.skipBits(24);
        } else {
          const int foregroundO = reader.readBits(2);
          const int foregroundR = reader.readBits(2);
          const int foregroundG = reader.readBits(2);
          const int foregroundB = reader.readBits(2);
          const int backgroundO = reader.readBits(2);
          const int backgroundR = reader.readBits(2);
          const int backgroundG = reader.readBits(2);
          const int backgroundB = reader.readBits(2);
          reader.skipBits(2);
          reader.readBits(2);
          reader.readBits(2);
          reader.readBits(2);
          currentWindow().setPenColor(
            CaptionWindow::argbFromCeaColor(foregroundR, foregroundG, foregroundB, foregroundO),
            CaptionWindow::argbFromCeaColor(backgroundR, backgroundG, backgroundB, backgroundO));
        }
        break;
      case kCommandSpl:
        if (!currentWindow().isDefined()) {
          reader.skipBits(16);
        } else {
          reader.skipBits(4);
          const int row = reader.readBits(4);
          reader.skipBits(2);
          reader.readBits(6);
          currentWindow().setPenLocation(row);
        }
        break;
      case kCommandSwa:
        if (!currentWindow().isDefined()) {
          reader.skipBits(32);
        } else {
          const int fillO = reader.readBits(2);
          const int fillR = reader.readBits(2);
          const int fillG = reader.readBits(2);
          const int fillB = reader.readBits(2);
          reader.readBits(2);
          reader.readBits(2);
          reader.readBits(2);
          reader.readBits(2);
          reader.readBit();
          reader.readBit();
          reader.readBits(2);
          reader.readBits(2);
          const int justification = reader.readBits(2);
          reader.skipBits(8);
          currentWindow().setWindowAttributes(
            CaptionWindow::argbFromCeaColor(fillR, fillG, fillB, fillO), justification);
        }
        break;
      default:
        if (command >= kCommandDf0 && command <= kCommandDf0 + 7) {
          const int window = command - kCommandDf0;
          defineWindow(window, reader);
          currentWindowIndex_ = window;
        }
        break;
    }
    static_cast<void>(results);
  }

  void defineWindow(const int index, BitReader& reader) {
    reader.skipBits(2);
    const bool visible = reader.readBit() != 0;
    reader.skipBits(2);
    const int priority = reader.readBits(3);
    const bool relativePositioning = reader.readBit() != 0;
    const int verticalAnchor = reader.readBits(7);
    const int horizontalAnchor = reader.readBits(8);
    const int anchorId = reader.readBits(4);
    const int rowCount = reader.readBits(4);
    reader.skipBits(2);
    reader.readBits(6);
    reader.skipBits(2);
    const int windowStyle = reader.readBits(3);
    const int penStyle = reader.readBits(3);
    windows_[index].defineWindow(visible, priority, relativePositioning, verticalAnchor,
      horizontalAnchor, rowCount, anchorId, windowStyle, penStyle);
  }

  void handleC2(const int command, BitReader& reader) {
    if (command <= 0x07) {
      return;
    }
    if (command <= 0x0F) {
      reader.skipBits(8);
    } else if (command <= 0x17) {
      reader.skipBits(16);
    } else {
      reader.skipBits(24);
    }
  }

  void handleC3(const int command, BitReader& reader) {
    if (command <= 0x87) {
      reader.skipBits(32);
    } else if (command <= 0x8F) {
      reader.skipBits(40);
    } else {
      reader.skipBits(2);
      const int length = reader.readBits(6);
      reader.skipBits(static_cast<size_t>(length) * 8);
    }
  }

  void handleG0(const int code) {
    currentWindow().append(code == 0x7F ? 0x266B : static_cast<uint32_t>(code));
  }

  void handleG1(const int code) {
    currentWindow().append(static_cast<uint32_t>(code));
  }

  void handleG2(const int code) {
    switch (code) {
      case 0x20: currentWindow().append(0x0020); break;
      case 0x21: currentWindow().append(0x00A0); break;
      case 0x25: currentWindow().append(0x2026); break;
      case 0x2A: currentWindow().append(0x0160); break;
      case 0x2C: currentWindow().append(0x0152); break;
      case 0x30: currentWindow().append(0x2588); break;
      case 0x31: currentWindow().append(0x2018); break;
      case 0x32: currentWindow().append(0x2019); break;
      case 0x33: currentWindow().append(0x201C); break;
      case 0x34: currentWindow().append(0x201D); break;
      case 0x35: currentWindow().append(0x2022); break;
      case 0x39: currentWindow().append(0x2122); break;
      case 0x3A: currentWindow().append(0x0161); break;
      case 0x3C: currentWindow().append(0x0153); break;
      case 0x3D: currentWindow().append(0x2120); break;
      case 0x3F: currentWindow().append(0x0178); break;
      case 0x76: currentWindow().append(0x215B); break;
      case 0x77: currentWindow().append(0x215C); break;
      case 0x78: currentWindow().append(0x215D); break;
      case 0x79: currentWindow().append(0x215E); break;
      case 0x7A: currentWindow().append(0x2502); break;
      case 0x7B: currentWindow().append(0x2510); break;
      case 0x7C: currentWindow().append(0x2514); break;
      case 0x7D: currentWindow().append(0x2500); break;
      case 0x7E: currentWindow().append(0x2518); break;
      case 0x7F: currentWindow().append(0x250C); break;
      default: break;
    }
  }

  void handleG3(const int code) {
    currentWindow().append(code == 0xA0 ? 0x33C4 : static_cast<uint32_t>('_'));
  }

  void emitCues(std::vector<AVCuesWithTiming>& results) {
    struct Entry final {
      AVCue cue;
      int priority = 0;
    };
    std::vector<Entry> entries;
    for (auto& window: windows_) {
      if (!window.isDefinedAndVisible() || window.isEmpty()) {
        continue;
      }
      Entry entry;
      if (window.build(entry.cue, entry.priority)) {
        entries.push_back(std::move(entry));
      }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
      return left.priority > right.priority;
    });
    AVCuesWithTiming result;
    for (auto& entry: entries) {
      result.cues.push_back(std::move(entry.cue));
    }
    result.startTimeUs = Definitions::kTimeUnset;
    result.durationUs = Definitions::kTimeUnset;
    if (delayUs_ > 0) {
      result.cueTimesUs.assign(result.cues.size(), delayUs_);
      delayUs_ = 0;
    }
    results.push_back(std::move(result));
  }

  CaptionWindow& currentWindow() {
    return windows_[currentWindowIndex_];
  }

  void resetWindows() {
    for (auto& window: windows_) {
      window.reset();
    }
    currentWindowIndex_ = 0;
  }

  std::array<CaptionWindow, kNumWindows> windows_;
  int currentWindowIndex_ = 0;
  int selectedService_ = 1;
  int64_t delayUs_ = 0;
  int previousSequence_ = -1;
  int sequence_ = 0;
  int packetSize_ = 0;
  bool packetActive_ = false;
  std::vector<uint8_t> packet_;
};

AVCea708Parser::AVCea708Parser() : AVCea708Parser(1) {
}

AVCea708Parser::AVCea708Parser(const int serviceNumber) : mImpl(new Impl()) {
  mImpl->setService(serviceNumber);
}

AVCea708Parser::~AVCea708Parser() = default;

bool AVCea708Parser::parse(const uint8_t* data, const size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) {
    return false;
  }
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVCea708Parser::reset() {
  mImpl->reset();
}

} // namespace coral
