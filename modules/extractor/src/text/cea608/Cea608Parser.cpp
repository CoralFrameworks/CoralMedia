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

#include <extractor/text/cea608/Cea608Parser.h>

#include "text/SubtitleTextUtil.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr int kRows = 15;
constexpr int kColumns = 32;
constexpr int kStyleItalics = 7;
constexpr int kStyleUnchanged = 8;

/** The row a Preamble Address Code selects. */
constexpr int kRowIndices[] = {11, 1, 3, 12, 14, 5, 7, 9};
constexpr int kColumnIndices[] = {0, 4, 8, 12, 16, 20, 24, 28};

constexpr uint32_t kStyleColors[] = {
  0xFFFFFFFFu,  // white
  0xFF00FF00u,  // green
  0xFF0000FFu,  // blue
  0xFF00FFFFu,  // cyan
  0xFFFF0000u,  // red
  0xFFFFFF00u,  // yellow
  0xFFFF00FFu,  // magenta
};

constexpr uint32_t kSpecialCharacters[] = {
  0x00AE, 0x00B0, 0x00BD, 0x00BF, 0x2122, 0x00A2, 0x00A3, 0x266A,
  0x00E0, 0x0020, 0x00E8, 0x00E2, 0x00EA, 0x00EE, 0x00F4, 0x00FB,
};

constexpr uint32_t kExtendedEsFr[] = {
  0x00C1, 0x00C9, 0x00D3, 0x00DA, 0x00DC, 0x00FC, 0x2018, 0x00A1,
  0x002A, 0x0027, 0x2014, 0x00A9, 0x2120, 0x2022, 0x201C, 0x201D,
  0x00C0, 0x00C2, 0x00C7, 0x00C8, 0x00CA, 0x00CB, 0x00EB, 0x00CE,
  0x00CF, 0x00EF, 0x00D4, 0x00D9, 0x00F9, 0x00DB, 0x00AB, 0x00BB,
};

constexpr uint32_t kExtendedPtDe[] = {
  0x00C3, 0x00E3, 0x00CD, 0x00CC, 0x00EC, 0x00D2, 0x00F2, 0x00D5,
  0x00F5, 0x007B, 0x007D, 0x005C, 0x005E, 0x005F, 0x007C, 0x007E,
  0x00C4, 0x00E4, 0x00D6, 0x00F6, 0x00DF, 0x00A5, 0x00A4, 0x2502,
  0x00C5, 0x00E5, 0x00D8, 0x00F8, 0x250C, 0x2510, 0x2514, 0x2518,
};

std::string utf8(const uint32_t codePoint) {
  std::string result;
  if (codePoint < 0x80) {
    result.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
  return result;
}

uint32_t basicCharacter(const uint8_t code) {
  if (code == 0x7B) return 0x00E7;  // ç
  if (code == 0x7C) return 0x00F7;  // ÷
  if (code == 0x7D) return 0x00D1;  // Ñ
  if (code == 0x7E) return 0x00F1;  // ñ
  if (code == 0x7F) return 0x25A0;  // black square
  return code;
}

/** One caption cell and the pen attributes that were active when it was written. */
struct Cell final {
  std::string text = " ";
  int color = 0;
  bool italic = false;
  bool underline = false;
};

using Screen = std::array<std::array<Cell, kColumns>, kRows>;

Screen emptyScreen() {
  Screen screen;
  for (auto& row: screen) {
    for (auto& cell: row) {
      cell = Cell{};
    }
  }
  return screen;
}

struct StyleRun final {
  size_t start = 0;
  size_t end = 0;
  int color = 0;
  bool italic = false;
  bool underline = false;
};

/** Builds the cue text and its spans from the displayed screen. */
bool buildCue(const Screen& screen, AVCue& cue) {
  std::string text;
  std::vector<StyleRun> runs;
  for (const auto& row: screen) {
    int end = kColumns;
    while (end > 0 && row[static_cast<size_t>(end - 1)].text == " ") {
      --end;
    }
    if (end == 0) {
      if (!text.empty()) text.push_back('\n');
      continue;
    }
    if (!text.empty()) text.push_back('\n');
    for (int column = 0; column < end; ++column) {
      const Cell& cell = row[static_cast<size_t>(column)];
      StyleRun run;
      run.start = text.size();
      text += cell.text;
      run.end = text.size();
      run.color = cell.color;
      run.italic = cell.italic;
      run.underline = cell.underline;
      runs.push_back(run);
    }
  }
  if (text.empty()) {
    return false;
  }
  cue.text = std::move(text);
  cue.textAlignment = AVCue::Alignment::kCenter;

  bool active = false;
  bool italic = false;
  bool underline = false;
  size_t start = 0;
  for (const auto& run: runs) {
    if (run.italic || run.underline) {
      if (!active) {
        active = true;
        italic = run.italic;
        underline = run.underline;
        start = run.start;
      } else if (run.italic != italic || run.underline != underline) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kTextStyle;
        span.start = start;
        span.end = run.start;
        span.style.italic = italic;
        span.style.underline = underline;
        cue.spans.push_back(std::move(span));
        italic = run.italic;
        underline = run.underline;
        start = run.start;
      }
    } else if (active) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kTextStyle;
      span.start = start;
      span.end = run.start;
      span.style.italic = italic;
      span.style.underline = underline;
      cue.spans.push_back(std::move(span));
      active = false;
    }
  }
  if (active) {
    AVCueSpan span;
    span.type = AVCueSpan::Type::kTextStyle;
    span.start = start;
    span.end = cue.text.size();
    span.style.italic = italic;
    span.style.underline = underline;
    cue.spans.push_back(std::move(span));
  }

  int color = 0;
  size_t colorStart = 0;
  bool colorActive = false;
  for (const auto& run: runs) {
    if (run.color != 0) {
      if (!colorActive) {
        colorActive = true;
        color = run.color;
        colorStart = run.start;
      } else if (run.color != color) {
        AVCueSpan span;
        span.type = AVCueSpan::Type::kForegroundColor;
        span.start = colorStart;
        span.end = run.start;
        span.color = kStyleColors[static_cast<size_t>(color)];
        cue.spans.push_back(std::move(span));
        color = run.color;
        colorStart = run.start;
      }
    } else if (colorActive) {
      AVCueSpan span;
      span.type = AVCueSpan::Type::kForegroundColor;
      span.start = colorStart;
      span.end = run.start;
      span.color = kStyleColors[static_cast<size_t>(color)];
      cue.spans.push_back(std::move(span));
      colorActive = false;
    }
  }
  if (colorActive) {
    AVCueSpan span;
    span.type = AVCueSpan::Type::kForegroundColor;
    span.start = colorStart;
    span.end = cue.text.size();
    span.color = kStyleColors[static_cast<size_t>(color)];
    cue.spans.push_back(std::move(span));
  }
  return true;
}

} // namespace

class AVCea608Parser::Impl final {
public:
  explicit Impl(const int selectedChannel)
      : selectedChannel_(selectedChannel == 2 ? 2 : 1) {
  }

  void feed(const uint8_t* data, size_t size, std::vector<AVCuesWithTiming>& results) {
    for (size_t position = 0; position + 2 <= size; position += 2) {
      const uint8_t first = data[position] & 0x7F;
      const uint8_t second = data[position + 1] & 0x7F;
      if (first == 0 && second == 0) continue;
      if (first >= 0x20) {
        if (currentChannel_ != selectedChannel_) continue;
        write(basicCharacter(first));
        if (second >= 0x20) write(basicCharacter(second));
        continue;
      }
      if (first >= 0x10) {
        controlPair(first, second, results);
      }
    }
  }

  void reset() {
    displayed_ = emptyScreen();
    nonDisplayed_ = emptyScreen();
    row_ = kRows - 1;
    column_ = 0;
    mode_ = Mode::kPopOn;
    style_ = 0;
    italic_ = false;
    underline_ = false;
    currentChannel_ = 1;
  }

private:
  enum class Mode { kPopOn, kPaintOn, kRollUp };

  void write(const uint32_t codePoint) {
    if (row_ < 0 || row_ >= kRows || column_ >= kColumns) return;
    Screen& screen = mode_ == Mode::kPopOn ? nonDisplayed_ : displayed_;
    Cell& cell = screen[static_cast<size_t>(row_)][static_cast<size_t>(column_)];
    cell.text = utf8(codePoint);
    cell.color = style_;
    cell.italic = italic_;
    cell.underline = underline_;
    ++column_;
  }

  void controlPair(const uint8_t first, const uint8_t second,
      std::vector<AVCuesWithTiming>& results) {
    // Bit 3 of a control code selects the channel. Text bytes do not carry it, so track the
    // channel named by the most recent control code and drop the other channel's commands.
    currentChannel_ = (first & 0x08) != 0 ? 2 : 1;
    if (currentChannel_ != selectedChannel_) {
      return;
    }
    if ((first & 0xF7) == 0x11 && (second & 0xF0) == 0x30) {
      write(kSpecialCharacters[static_cast<size_t>(second & 0x0F)]);
      return;
    }
    if ((first & 0xF6) == 0x12 && (second & 0xE0) == 0x20) {
      const uint32_t* table = (first & 0x01) == 0 ? kExtendedEsFr : kExtendedPtDe;
      write(table[static_cast<size_t>(second & 0x1F)]);
      return;
    }
    if ((first & 0xF7) == 0x11 && (second & 0xF0) == 0x20) {
      write(0x20);
      setStyle((second >> 1) & 0x07, (second & 0x01) != 0);
      return;
    }
    if ((first & 0xF0) == 0x10 && (second & 0xC0) == 0x40) {
      int row = kRowIndices[first & 0x07];
      if ((second & 0x20) != 0) ++row;
      if (row != row_) {
        row_ = row;
        column_ = 0;
      }
      const bool isCursor = (second & 0x10) != 0;
      const bool underline = (second & 0x01) != 0;
      const int style = (second >> 1) & 0x07;
      if (isCursor) {
        column_ = kColumnIndices[static_cast<size_t>(style)];
        underline_ = underline;
      } else {
        setStyle(style, underline);
      }
      return;
    }
    if ((first & 0xF7) == 0x17 && second >= 0x21 && second <= 0x23) {
      column_ = std::min(kColumns, column_ + static_cast<int>(second - 0x20));
      return;
    }
    if ((first & 0xF6) == 0x14) {
      control(second, results);
    }
  }

  void setStyle(const int style, const bool underline) {
    underline_ = underline;
    if (style == kStyleUnchanged) return;
    if (style == kStyleItalics) {
      italic_ = true;
      return;
    }
    italic_ = false;
    style_ = style >= 0 && style < 7 ? style : 0;
  }

  const int selectedChannel_;
  int currentChannel_ = 1;

  void control(const uint8_t command, std::vector<AVCuesWithTiming>& results) {
    switch (command) {
      case 0x20:
        mode_ = Mode::kPopOn;
        row_ = kRows - 1;
        column_ = 0;
        nonDisplayed_ = emptyScreen();
        break;
      case 0x21:
        if (column_ > 0) --column_;
        break;
      case 0x24:
        if (row_ >= 0 && row_ < kRows) {
          displayed_[static_cast<size_t>(row_)] = {};
          for (auto& cell: displayed_[static_cast<size_t>(row_)]) cell = Cell{};
          if (mode_ == Mode::kPopOn) {
            nonDisplayed_[static_cast<size_t>(row_)] = {};
            for (auto& cell: nonDisplayed_[static_cast<size_t>(row_)]) cell = Cell{};
          }
        }
        break;
      case 0x25:
      case 0x26:
      case 0x27:
        mode_ = Mode::kRollUp;
        row_ = kRows - 1;
        column_ = 0;
        break;
      case 0x29:
        mode_ = Mode::kPaintOn;
        break;
      case 0x2C:
        displayed_ = emptyScreen();
        break;
      case 0x2D:
        if (mode_ == Mode::kRollUp) {
          for (int row = 0; row + 1 < kRows; ++row) {
            displayed_[static_cast<size_t>(row)] = displayed_[static_cast<size_t>(row + 1)];
          }
          displayed_[kRows - 1] = {};
          for (auto& cell: displayed_[kRows - 1]) cell = Cell{};
          column_ = 0;
          emit(results);
        }
        break;
      case 0x2E:
        nonDisplayed_ = emptyScreen();
        break;
      case 0x2F:
        std::swap(displayed_, nonDisplayed_);
        emit(results);
        break;
      default:
        break;
    }
  }

  void emit(std::vector<AVCuesWithTiming>& results) {
    AVCue cue;
    if (!buildCue(displayed_, cue)) return;
    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = Definitions::kTimeUnset;
    result.durationUs = Definitions::kTimeUnset;
    results.push_back(std::move(result));
  }

  Screen displayed_ = emptyScreen();
  Screen nonDisplayed_ = emptyScreen();
  int row_ = kRows - 1;
  int column_ = 0;
  Mode mode_ = Mode::kPopOn;
  int style_ = 0;
  bool italic_ = false;
  bool underline_ = false;
};

AVCea608Parser::AVCea608Parser() : AVCea608Parser(1) {
}

AVCea608Parser::AVCea608Parser(const int selectedChannel)
    : mImpl(new Impl(selectedChannel)) {
}

AVCea608Parser::~AVCea608Parser() = default;

bool AVCea608Parser::parse(const uint8_t* data, size_t size, const OutputOptions& options,
    const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) return false;
  std::vector<AVCuesWithTiming> results;
  mImpl->feed(data, size, results);
  coral::text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues,
    output);
  return true;
}

void AVCea608Parser::reset() {
  mImpl->reset();
}

} // namespace coral
