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

#ifndef CORAL_EXTRACTOR_SRC_TEXT_SUBTITLE_TEXT_UTIL_H
#define CORAL_EXTRACTOR_SRC_TEXT_SUBTITLE_TEXT_UTIL_H

#include <media/Definitions.h>

#include <extractor/text/CuesWithTiming.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coral {
namespace text {

/**
 * A line reader over a block of subtitle bytes, decoding UTF-8 and UTF-16.
 *
 * This is the internal counterpart of the `ParsableByteArray` of ExoPlayer with its `readLine`
 * overloads: a subtitle file is a sequence of lines, and a file that states its encoding with a byte
 * order mark must be decoded with it. The reader owns no bytes: it is a cursor over the buffer the
 * extractor handed in, so a parser never copies the whole sample.
 */
class Reader final {
public:
  Reader(const uint8_t* data, size_t size);

  /**
   * Reads the next line, without its terminator.
   *
   * @param line Set to the line in UTF-8, with the `\r` of a CRLF terminator removed.
   *
   * @return {@code false} once the end of the buffer was reached.
   */
  bool readLine(std::string& line);

  /** Returns whether every byte was consumed. */
  [[nodiscard]] bool isAtEnd() const { return mPosition >= mSize; }

  /** Returns the position of the next byte to read. */
  [[nodiscard]] size_t position() const { return mPosition; }

  /** Returns the number of bytes of the buffer. */
  [[nodiscard]] size_t size() const { return mSize; }

  /** Returns whether the first byte that was read was a byte order mark. */
  [[nodiscard]] bool hadByteOrderMark() const { return mHadByteOrderMark; }

private:
  /** Reads one code unit of the source encoding, or returns false at the end. */
  bool readCodeUnit(uint32_t& codeUnit);

  const uint8_t* mData;
  size_t mSize;
  size_t mPosition;

  /** Whether the buffer is UTF-16 rather than UTF-8. */
  bool mUtf16;
  /** Whether the UTF-16 code units are big endian. */
  bool mUtf16BigEndian;
  bool mHadByteOrderMark;
};

/** Removes the leading and trailing whitespace of @p value. */
std::string trim(std::string_view value);

/** Returns @p value converted to lower case, for ASCII characters only. */
std::string toLowerAscii(std::string_view value);

/** Returns whether @p value starts with @p prefix, ignoring the case of ASCII letters. */
bool startsWithIgnoreCase(std::string_view value, std::string_view prefix);

/** Returns whether @p value ends with @p suffix. */
bool endsWith(std::string_view value, std::string_view suffix);

/**
 * Splits @p value on @p delimiter.
 *
 * <p>With a non zero @p limit the result holds at most `limit` fields and the last field keeps the
 * remainder of the string, which is the behaviour of Java's `String.split(regex, limit)` that the
 * SSA parser relies on to keep commas inside the text of a dialogue line: there is no escape for a
 * comma, so only the number of fields the format line declares tells where the text starts.
 */
std::vector<std::string> split(std::string_view value, char delimiter, size_t limit = 0);

/** Parses a signed decimal integer that spans the whole of @p value. */
bool parseInteger(std::string_view value, int64_t& out);

/** Parses a decimal floating point number that spans the whole of @p value. */
bool parseFloat(std::string_view value, float& out);

/**
 * Parses a WebVTT timestamp (`hh:mm:ss.mmm`, `mm:ss.mmm` or `ss.mmm`) into microseconds.
 *
 * <p>The fractional part must hold exactly three digits, as the WebVTT specification requires.
 */
bool parseWebvttTimestamp(std::string_view value, int64_t& outUs);

/**
 * Parses a SubRip timestamp (`hh:mm:ss,mmm`, with optional hours and milliseconds) into
 * microseconds.
 */
bool parseSubripTimestamp(std::string_view value, int64_t& outUs);

/**
 * Parses an SSA/ASS timestamp (`h:mm:ss.cc`) into microseconds.
 *
 * <p>The last field is a fraction of a second with as many digits as the file states, usually two
 * hundredths, and is scaled accordingly rather than assumed to be centiseconds.
 */
bool parseSsaTimestamp(std::string_view value, int64_t& outUs);

/** Parses a WebVTT percentage (`50%`) into a fraction, i.e. 0.5 for `50%`. */
bool parsePercentage(std::string_view value, float& outFraction);

/**
 * Appends the UTF-8 encoding of the UTF-16 code units of @p value to @p out.
 *
 * A lone surrogate is replaced by U+FFFD rather than dropped, so that text that a malformed file
 * encoded as UTF-16 still shows something.
 */
void appendUtf16AsUtf8(std::string& out, const uint8_t* data, size_t size, bool bigEndian);

/**
 * Emits the results of one parse call in the order that the output options of a parser ask for.
 *
 * <p>A result that states its own times is wanted once its interval reaches the requested start;
 * the ones that precede it are emitted afterwards when the options ask for every cue. A result
 * without times of its own belongs to the sample and is always emitted.
 *
 * @param results The results to emit, in the order of the data.
 * @param startTimeUs The requested start, or {@link Definitions::kTimeUnset} for every cue.
 * @param outputAllCues Whether the results before the start are emitted after the others.
 * @param output The consumer.
 */
void emitInRequestedOrder(std::vector<AVCuesWithTiming> results, int64_t startTimeUs,
                          bool outputAllCues,
                          const std::function<void(const AVCuesWithTiming&)>& output);

/**
 * Inflates @p data when it starts with the zlib header, as a compressed bitmap subtitle sample does.
 *
 * @param data The encoded sample.
 * @param size The number of bytes in @p data.
 * @param output Set to the inflated bytes, or cleared when the sample is not zlib or is corrupt.
 *
 * @return true when @p data was zlib and was inflated.
 */
bool maybeInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& output);

} // namespace text
} // namespace coral

#endif //CORAL_EXTRACTOR_SRC_TEXT_SUBTITLE_TEXT_UTIL_H
