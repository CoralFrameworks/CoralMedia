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

#ifndef CORAL_EXTRACTOR_UTILITY_H
#define CORAL_EXTRACTOR_UTILITY_H

#include <extractor/DataBuffer.h>
#include <extractor/ExtractorInput.h>

#include <media/Definitions.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coral {

/**
 * The extractor module counterpart of ExoPlayer's {@code Util}.
 */
class AVUtil final {
public:
  /**
   * The number of microseconds in a second.
   */
  static constexpr int64_t kMicrosPerSecond = 1000000;

  /**
   * The number of microseconds in a millisecond.
   */
  static constexpr int64_t kMicrosPerMilli = 1000;

  /**
   * The maximum value of a signed 32 bit integer.
   */
  static constexpr int kMaxInt = 0x7FFFFFFF;

  /**
   * The number of bytes of the start code that separates the NAL units of an Annex-B sample.
   */
  static constexpr size_t kStartCodeSize = 4;

  /**
   * Returns the number of bytes needed to encode the given number of unsigned bits.
   */
  static constexpr int bytesToBits(const int bytes) { return bytes * 8; }

  /**
   * Reads an unsigned big endian integer from four bytes.
   */
  static constexpr uint32_t readUnsignedInt(const uint8_t data[], const int offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
  }

  /**
   * Reads an unsigned big endian integer from three bytes.
   */
  static constexpr uint32_t readUnsignedInt24(const uint8_t data[], const int offset) {
    return (static_cast<uint32_t>(data[offset]) << 16) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           static_cast<uint32_t>(data[offset + 2]);
  }

  /**
   * Reads an unsigned big endian short from two bytes.
   */
  static constexpr uint16_t readUnsignedShort(const uint8_t data[], const int offset) {
    return static_cast<uint16_t>((static_cast<uint32_t>(data[offset]) << 8) |
                                 static_cast<uint32_t>(data[offset + 1]));
  }

  /**
   * Converts a sample count and a sample rate to a duration in microseconds.
   */
  static int64_t scaleToUs(const int64_t value, const int64_t fromScale) {
    if (fromScale == 0) {
      return Definitions::kTimeUnset;
    }
    return static_cast<int64_t>((static_cast<double>(value) * kMicrosPerSecond) /
                                static_cast<double>(fromScale));
  }

  /**
   * Converts a value expressed in a scale to another scale.
   */
  static int64_t scaleLargeValue(const int64_t value, const int64_t fromScale,
                                 const int64_t toScale) {
    if (fromScale == toScale) {
      return value;
    }
    if (fromScale == 0) {
      return 0;
    }
    return static_cast<int64_t>((static_cast<double>(value) * static_cast<double>(toScale)) /
                                static_cast<double>(fromScale));
  }

  /**
   * Returns the greatest common divisor of two positive values.
   */
  static int64_t gcd(int64_t a, int64_t b);

  /**
   * Returns the least common multiple of two positive values, capped at {@code max}.
   */
  static int64_t lcm(const int64_t a, const int64_t b, const int64_t max);

  /**
   * Searches for the first occurrence of a four byte identifier in the given buffer.
   *
   * @return The index of the occurrence, or -1.
   */
  static int indexOf(const uint8_t data[], size_t size, const uint32_t fourCc);

  /**
   * Returns whether the given buffer starts with the given four byte identifier.
   */
  static bool startsWith(const uint8_t data[], size_t size, const uint32_t fourCc);

  /**
   * Returns the size of the Annex-B framing of a sample whose NAL units are length prefixed, or 0 when
   * the sample is not length prefixed.
   *
   * <p>A sample of a container that stores its NAL units with a length prefix, e.g. an AVC or HEVC
   * track of an MP4 file, uses the length prefix the configuration record declares. The framing is
   * converted to the one the platform decoders read, which is what the {@code h264_mp4toannexb} filter
   * of the platform extractor does as well.
   *
   * @param data The sample.
   * @param size The size of the sample in bytes.
   * @param lengthSize The number of bytes of the length prefix of every NAL unit, between 1 and 4.
   * @return The number of bytes of the converted sample, or 0 when the sample is not length prefixed.
   *         A sample that already starts with a start code is not converted.
   */
  static size_t annexBSize(const uint8_t data[], size_t size, int lengthSize);

  /**
   * Writes the Annex-B framing of a length prefixed sample into the given buffer, which has to hold
   * {@link annexBSize} bytes.
   */
  static void toAnnexB(const uint8_t data[], size_t size, int lengthSize, uint8_t target[]);

  /**
   * Peeks a four byte identifier at the given offset.
   *
   * @return The identifier, or -1 when the input is too short.
   */
  static int peekFourCc(AVExtractorInput& input, int offset);

private:
  AVUtil() = default;

  ~AVUtil() = default;
};

/**
 * A minimal big endian bit reader, used for the codec configuration records that are not byte
 * aligned, e.g. the AAC AudioSpecificConfig.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code ParsableBitArray}. It never
 * owns the data it reads, therefore the caller must keep the buffer alive, which is enforced by
 * taking a shared pointer to it.
 */
class AVBitReader final {
public:
  AVBitReader() noexcept = default;

  explicit AVBitReader(std::shared_ptr<AVDataBuffer> data) noexcept
    : data_(std::move(data)) {
  }

  AVBitReader(const uint8_t* data, const size_t size) noexcept
    : AVBitReader(std::make_shared<AVDataBuffer>(data, static_cast<uint32_t>(size))) {
  }

  ~AVBitReader() = default;

  AVBitReader(const AVBitReader& other) = delete;

  AVBitReader& operator=(const AVBitReader& other) = delete;

  /**
   * Returns whether at least {@code numBits} bits are available.
   */
  [[nodiscard]] bool canReadBits(const int numBits) const {
    const size_t totalBits = data_ != nullptr ? data_->limit() * 8 : 0;
    return static_cast<size_t>(bitPosition_) + static_cast<size_t>(numBits) <= totalBits;
  }

  /**
   * Reads {@code numBits} bits, and returns them in the least significant bits of the result.
   *
   * @param numBits The number of bits to read, at most 32. Must be available.
   */
  [[nodiscard]] uint32_t readBits(const int numBits);

  /**
   * Reads {@code numBits} bits and sign extends the result.
   */
  [[nodiscard]] int32_t readSignedBits(const int numBits) {
    const uint32_t value = readBits(numBits);
    const uint32_t signBit = 1U << (numBits - 1);
    return static_cast<int32_t>((value ^ signBit) - signBit);
  }

  /**
   * Skips {@code numBits} bits.
   */
  void skipBits(const int numBits) { bitPosition_ += numBits; }

  /**
   * Returns the current bit position.
   */
  [[nodiscard]] int getPosition() const { return bitPosition_; }

  /**
   * Sets the current bit position.
   */
  void setPosition(const int position) { bitPosition_ = position; }

  /**
   * Returns the number of bits that are available in total.
   */
  [[nodiscard]] int getTotalBits() const {
    return data_ != nullptr ? static_cast<int>(data_->limit() * 8) : 0;
  }

private:
  std::shared_ptr<AVDataBuffer> data_;
  int bitPosition_ = 0;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_UTILITY_H
