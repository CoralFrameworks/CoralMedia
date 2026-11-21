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

#include "SubtitleTextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <zlib.h>

namespace coral {
namespace text {
namespace {

constexpr uint32_t kReplacementCharacter = 0xFFFD;

/** Appends the UTF-8 encoding of @p codePoint to @p out. */
void appendUtf8(std::string& out, const uint32_t codePoint) {
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

/** The factor that converts a fraction of a second to microseconds. */
int64_t fractionToMicros(const uint64_t fraction, const size_t digits) {
  if (digits == 0 || digits > 9) {
    return 0;
  }
  static constexpr int64_t kPowersOfTen[] = {1, 10, 100, 1000, 10000, 100000,
                                             1000000, 10000000, 100000000, 1000000000};
  const int64_t scale = kPowersOfTen[digits];
  // A fraction finer than a microsecond is rounded down: no subtitle of a container has a time
  // base that fine, and rounding keeps the parse total.
  return static_cast<int64_t>(fraction) * 1000000 / scale;
}

} // namespace

Reader::Reader(const uint8_t* data, const size_t size)
  : mData(data),
    mSize(size),
    mPosition(0),
    mUtf16(false),
    mUtf16BigEndian(false),
    mHadByteOrderMark(false) {
  if (data != nullptr && size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    mHadByteOrderMark = true;
    mPosition = 3;
  } else if (data != nullptr && size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
    mHadByteOrderMark = true;
    mUtf16 = true;
    mUtf16BigEndian = false;
    mPosition = 2;
  } else if (data != nullptr && size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    mHadByteOrderMark = true;
    mUtf16 = true;
    mUtf16BigEndian = true;
    mPosition = 2;
  }
}


bool Reader::readCodeUnit(uint32_t& codeUnit) {
  if (mData == nullptr || mPosition + 2 > mSize) {
    return false;
  }
  if (mUtf16BigEndian) {
    codeUnit = (static_cast<uint32_t>(mData[mPosition]) << 8) | mData[mPosition + 1];
  } else {
    codeUnit = static_cast<uint32_t>(mData[mPosition]) |
               (static_cast<uint32_t>(mData[mPosition + 1]) << 8);
  }
  mPosition += 2;
  return true;
}

bool Reader::readLine(std::string& line) {
  line.clear();
  if (mData == nullptr || mPosition >= mSize) {
    return false;
  }
  if (!mUtf16) {
    const size_t start = mPosition;
    while (mPosition < mSize && mData[mPosition] != '\n') {
      ++mPosition;
    }
    line.assign(reinterpret_cast<const char*>(mData) + start, mPosition - start);
    if (mPosition < mSize) {
      ++mPosition;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return true;
  }

  uint32_t codeUnit = 0;
  while (readCodeUnit(codeUnit)) {
    if (codeUnit == 0x000A) {
      // The terminator is consumed, not part of the line.
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return true;
    }
    if (codeUnit == 0x000D) {
      // A CRLF pair: remember it, the LF that follows ends the line.
      line.push_back('\r');
      continue;
    }
    if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
      const size_t pairPosition = mPosition;
      uint32_t low = 0;
      if (readCodeUnit(low) && low >= 0xDC00 && low <= 0xDFFF) {
        const uint32_t codePoint = 0x10000 + ((codeUnit - 0xD800) << 10) + (low - 0xDC00);
        appendUtf8(line, codePoint);
        continue;
      }
      mPosition = pairPosition;
      appendUtf8(line, kReplacementCharacter);
      continue;
    }
    if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) {
      appendUtf8(line, kReplacementCharacter);
      continue;
    }
    appendUtf8(line, codeUnit);
  }

  // The buffer ended in the middle of a line: the line that was read so far is the last one.
  if (!line.empty()) {
    if (line.back() == '\r') {
      line.pop_back();
    }
    return true;
  }
  return false;
}

std::string trim(const std::string_view value) {
  size_t start = 0;
  size_t end = value.size();
  const auto isSpace = [](const char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
           character == '\f' || character == '\v';
  };
  while (start < end && isSpace(value[start])) {
    ++start;
  }
  while (end > start && isSpace(value[end - 1])) {
    --end;
  }
  return std::string(value.substr(start, end - start));
}

std::string toLowerAscii(const std::string_view value) {
  std::string lower(value);
  for (char& character : lower) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lower;
}

bool startsWithIgnoreCase(const std::string_view value, const std::string_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    char left = value[i];
    char right = prefix[i];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

bool endsWith(const std::string_view value, const std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string_view value, const char delimiter,
                               const size_t limit) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    if (limit != 0 && fields.size() + 1 >= limit) {
      fields.emplace_back(value.substr(start));
      break;
    }
    const size_t separator = value.find(delimiter, start);
    if (separator == std::string_view::npos) {
      fields.emplace_back(value.substr(start));
      break;
    }
    fields.emplace_back(value.substr(start, separator - start));
    start = separator + 1;
  }
  if (limit == 0) {
    // Java's `String.split` drops the trailing empty fields when no limit was asked for.
    while (!fields.empty() && fields.back().empty()) {
      fields.pop_back();
    }
  }
  return fields;
}

bool parseInteger(const std::string_view value, int64_t& out) {
  if (value.empty()) {
    return false;
  }
  int64_t parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  out = parsed;
  return true;
}

bool parseFloat(const std::string_view value, float& out) {
  if (value.empty()) {
    return false;
  }
  float parsed = 0.0f;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || !std::isfinite(parsed)) {
    return false;
  }
  out = parsed;
  return true;
}

bool parseWebvttTimestamp(const std::string_view value, int64_t& outUs) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return false;
  }
  const size_t dot = trimmed.find('.');
  const std::string_view whole = dot == std::string::npos
                                   ? std::string_view(trimmed)
                                   : std::string_view(trimmed).substr(0, dot);
  const std::string_view fraction = dot == std::string::npos
                                      ? std::string_view()
                                      : std::string_view(trimmed).substr(dot + 1);
  const std::vector<std::string> parts = split(whole, ':');
  if (parts.empty() || parts.size() > 3) {
    return false;
  }
  int64_t seconds = 0;
  for (const auto& part : parts) {
    int64_t component = 0;
    if (!parseInteger(part, component) || component < 0) {
      return false;
    }
    seconds = seconds * 60 + component;
  }
  int64_t fractionUs = 0;
  if (dot != std::string::npos) {
    if (fraction.size() != 3) {
      return false;
    }
    int64_t millis = 0;
    if (!parseInteger(fraction, millis)) {
      return false;
    }
    fractionUs = millis * 1000;
  }
  if (seconds > (std::numeric_limits<int64_t>::max() - fractionUs) / 1000000) {
    return false;
  }
  outUs = seconds * 1000000 + fractionUs;
  return true;
}

bool parseSubripTimestamp(const std::string_view value, int64_t& outUs) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return false;
  }
  const size_t comma = trimmed.find(',');
  const std::string_view whole = comma == std::string::npos
                                   ? std::string_view(trimmed)
                                   : std::string_view(trimmed).substr(0, comma);
  const std::string_view fraction = comma == std::string::npos
                                      ? std::string_view()
                                      : std::string_view(trimmed).substr(comma + 1);
  const std::vector<std::string> parts = split(whole, ':');
  if (parts.size() != 2 && parts.size() != 3) {
    return false;
  }
  int64_t hours = 0;
  if (parts.size() == 3 && !parseInteger(parts[0], hours)) {
    return false;
  }
  int64_t minutes = 0;
  int64_t seconds = 0;
  if (!parseInteger(parts[parts.size() - 2], minutes) ||
      !parseInteger(parts[parts.size() - 1], seconds)) {
    return false;
  }
  int64_t fractionUs = 0;
  if (comma != std::string::npos) {
    if (fraction.empty() || fraction.size() > 3) {
      return false;
    }
    int64_t millis = 0;
    if (!parseInteger(fraction, millis)) {
      return false;
    }
    // `,5` is five tenths of a second, not five milliseconds.
    millis *= fraction.size() == 1 ? 100 : (fraction.size() == 2 ? 10 : 1);
    fractionUs = millis * 1000;
  }
  const int64_t totalSeconds = (hours * 60 + minutes) * 60 + seconds;
  if (totalSeconds > (std::numeric_limits<int64_t>::max() - fractionUs) / 1000000) {
    return false;
  }
  outUs = totalSeconds * 1000000 + fractionUs;
  return true;
}

bool parseSsaTimestamp(const std::string_view value, int64_t& outUs) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return false;
  }
  const std::vector<std::string> parts = split(trimmed, ':');
  if (parts.size() != 2 && parts.size() != 3) {
    return false;
  }
  int64_t hours = 0;
  if (parts.size() == 3 && !parseInteger(parts[0], hours)) {
    return false;
  }
  int64_t minutes = 0;
  if (!parseInteger(parts[parts.size() - 2], minutes)) {
    return false;
  }
  const std::string& lastPart = parts[parts.size() - 1];
  const size_t separator = lastPart.find_first_of(".:");
  int64_t seconds = 0;
  uint64_t fraction = 0;
  size_t fractionDigits = 0;
  if (separator == std::string::npos) {
    if (!parseInteger(lastPart, seconds)) {
      return false;
    }
  } else {
    if (!parseInteger(std::string_view(lastPart).substr(0, separator), seconds)) {
      return false;
    }
    const std::string_view fractionText = std::string_view(lastPart).substr(separator + 1);
    if (fractionText.empty() || fractionText.size() > 9) {
      return false;
    }
    for (const char character : fractionText) {
      if (character < '0' || character > '9') {
        return false;
      }
      fraction = fraction * 10 + static_cast<uint64_t>(character - '0');
    }
    fractionDigits = fractionText.size();
  }
  const int64_t totalSeconds = (hours * 60 + minutes) * 60 + seconds;
  const int64_t fractionUs = fractionToMicros(fraction, fractionDigits);
  if (totalSeconds > (std::numeric_limits<int64_t>::max() - fractionUs) / 1000000) {
    return false;
  }
  outUs = totalSeconds * 1000000 + fractionUs;
  return true;
}

bool parsePercentage(const std::string_view value, float& outFraction) {
  if (!endsWith(value, "%")) {
    return false;
  }
  float percentage = 0.0f;
  if (!parseFloat(trim(std::string_view(value).substr(0, value.size() - 1)), percentage)) {
    return false;
  }
  outFraction = percentage / 100.0f;
  return true;
}

void appendUtf16AsUtf8(std::string& out, const uint8_t* data, const size_t size,
                       const bool bigEndian) {
  if (data == nullptr) {
    return;
  }
  const auto readUnit = [&](const size_t offset, uint32_t& unit) {
    if (offset + 2 > size) {
      return false;
    }
    unit = bigEndian ? (static_cast<uint32_t>(data[offset]) << 8) | data[offset + 1]
                     : static_cast<uint32_t>(data[offset]) |
                         (static_cast<uint32_t>(data[offset + 1]) << 8);
    return true;
  };
  size_t offset = 0;
  uint32_t unit = 0;
  while (readUnit(offset, unit)) {
    offset += 2;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      uint32_t low = 0;
      if (readUnit(offset, low) && low >= 0xDC00 && low <= 0xDFFF) {
        offset += 2;
        appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
        continue;
      }
      appendUtf8(out, kReplacementCharacter);
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      appendUtf8(out, kReplacementCharacter);
      continue;
    }
    appendUtf8(out, unit);
  }
}

void emitInRequestedOrder(std::vector<AVCuesWithTiming> results, const int64_t startTimeUs,
                          const bool outputAllCues,
                          const std::function<void(const AVCuesWithTiming&)>& output) {
  if (output == nullptr) {
    return;
  }
  if (startTimeUs == Definitions::kTimeUnset) {
    for (auto& result : results) {
      output(result);
    }
    return;
  }
  std::vector<AVCuesWithTiming> deferred;
  for (auto& result : results) {
    // A result without a time of its own belongs to the sample, which the caller always wants; one
    // that states its times is wanted once its interval reaches the requested start.
    const bool afterStart = result.startTimeUs == Definitions::kTimeUnset ||
                            result.startTimeUs >= startTimeUs ||
                            (result.durationUs > 0 && result.startTimeUs + result.durationUs >= startTimeUs);
    if (afterStart) {
      output(result);
    } else if (outputAllCues) {
      deferred.push_back(std::move(result));
    }
  }
  for (auto& result : deferred) {
    output(result);
  }
}

bool maybeInflate(const uint8_t* data, const size_t size, std::vector<uint8_t>& output) {
  output.clear();
  if (data == nullptr || size < 2 || data[0] != 0x78 || size > std::numeric_limits<uInt>::max()) {
    return false;
  }
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) {
    return false;
  }
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  output.resize(std::max<size_t>(size * 4, 64 * 1024));
  constexpr size_t kMaxInflatedSize = 64 * 1024 * 1024;
  int status = Z_OK;
  while (status == Z_OK) {
    if (stream.total_out == output.size()) {
      if (output.size() >= kMaxInflatedSize) {
        status = Z_MEM_ERROR;
        break;
      }
      output.resize(std::min(output.size() * 2, kMaxInflatedSize));
    }
    stream.next_out = output.data() + stream.total_out;
    stream.avail_out = static_cast<uInt>(output.size() - stream.total_out);
    const uLong before = stream.total_out;
    status = inflate(&stream, Z_NO_FLUSH);
    if (status == Z_BUF_ERROR && stream.total_out == before) {
      if (output.size() >= kMaxInflatedSize) {
        status = Z_MEM_ERROR;
        break;
      }
      output.resize(std::min(output.size() * 2, kMaxInflatedSize));
      status = Z_OK;
    }
  }
  const size_t inflatedSize = stream.total_out;
  inflateEnd(&stream);
  if (status != Z_STREAM_END) {
    output.clear();
    return false;
  }
  output.resize(inflatedSize);
  return true;
}

} // namespace text
} // namespace coral
