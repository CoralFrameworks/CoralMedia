/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_CODEC_TYPES_H
#define CORAL_CODEC_TYPES_H

#include <cstdint>
#include <string>

namespace coral {

/**
 * Collection of codec manufacturer
 */
enum class CodecManufacturer : uint32_t {
  //!< Codec software manufacturer
  kSoftware = 0,
  //!< Codec hardware manufacturer
  kHardware = 1,
};

/**
 * The format of the muxer
 */
enum class MuxerFormat : int8_t {
  //!< unsupported media file format
  kMuxerFormatInvalid = -1,
  //!< MPEG4 media file format
  kMuxerFormatMpeg4 = 0,
  //!< WEBM media file format
  kMuxerFormatWebm = 1,
  //!< 3GPP media file format
  kMuxerFormat3Gpp = 2,
  //!< Ogg media file format
  kMuxerFormatOgg = 3,
  //!< Opus media file format
  kMuxerFormatOpus = 4,
  //!< Flac media file format
  kMuxerFormatFlac = 5,
  //!< h263 media file format
  kMuxerFormatH263 = 6,
  //!< h264 media file format
  kMuxerFormatH264 = 7,
};


/**
 * Defines a seek point in a media stream.
 */
struct SeekPoint {
  /**
   * The time of the seek point, in microseconds.
   */
  int64_t timeUs;
  /**
   * The byte offset of the seek point.
   */
  int64_t position;

  explicit SeekPoint(const int64_t timeUs, const int64_t position) noexcept
      : timeUs(timeUs),
        position(position) {
  }

  SeekPoint(const SeekPoint& other) = default;
  SeekPoint& operator=(const SeekPoint& other) = default;

  SeekPoint(SeekPoint&& other) noexcept = default;
  SeekPoint& operator=(SeekPoint&& other) noexcept = default;

  [[nodiscard]] std::string toString() const {
    std::string str("{timeUs=");
    str.append(std::to_string(timeUs))
       .append(", position=")
       .append(std::to_string(position))
       .append("}");
    return str;
  }

  friend bool operator==(const SeekPoint& lhs, const SeekPoint& rhs) {
    return lhs.timeUs == rhs.timeUs && lhs.position == rhs.position;
  }

  friend bool operator!=(const SeekPoint& lhs, const SeekPoint& rhs) {
    return !(lhs == rhs);
  }
};

/**
 * Contains one or two {@link SeekPoint}s.
 */
struct SeekPoints {
  /**
   * The first seek point.
   */
  SeekPoint first;
  /**
   * The second seek point, or {@link first} if there's only one seek point.
   */
  SeekPoint second;

  explicit SeekPoints(const SeekPoint& point) noexcept
      : SeekPoints(point, point) {
  }

  explicit SeekPoints(const SeekPoint& first, const SeekPoint& second) noexcept
      : first(first),
        second(second) {
  }

  SeekPoints(const SeekPoints& other) = default;
  SeekPoints& operator=(const SeekPoints& other) = default;

  SeekPoints(SeekPoints&& other) noexcept = default;
  SeekPoints& operator=(SeekPoints&& other) noexcept = default;

  [[nodiscard]] std::string toString() const {
    std::string str("[");
    str.append(first.toString())
       .append(first == second ? "" : ", ")
       .append(first != second ? second.toString() : "")
       .append("]");
    return str;
  }

  friend bool operator==(const SeekPoints& lhs, const SeekPoints& rhs) {
    return lhs.first == rhs.first && lhs.second == rhs.second;
  }

  friend bool operator!=(const SeekPoints& lhs, const SeekPoints& rhs) {
    return !(lhs == rhs);
  }
};

/**
 * Maps seek positions (in microseconds) to corresponding positions (byte offsets) in the stream.
 */
class SeekMap {
public:
  virtual ~SeekMap() noexcept;

  /**
   * Returns whether seeking is supported.
   *
   * @return Whether seeking is supported.
   */
  [[nodiscard]] virtual bool isSeekable() const = 0;

  /**
   * Returns the duration of the stream in microseconds.
   *
   * @return The duration of the stream in microseconds, or {@link Definitions::kTimeUnset} if the
   *     duration is unknown.
   */
  [[nodiscard]] virtual int64_t getDurationUs() const = 0;

  /**
   * Obtains seek points for the specified seek time in microseconds. The returned {@link
   * SeekPoints} will contain one or two distinct seek points.
   *
   * Two seek points [A, B] are returned in the case that seeking can only be performed to discrete
   * points in time, there does not exist a seek point at exactly the requested time, and there
   * exist seek points on both sides of it. In this case A and B are the closest seek points
   * before and after the requested time. A single seek point is returned in all other cases.
   *
   * @param timeUs A seek time in microseconds.
   * @return The corresponding seek points.
   */
  virtual SeekPoints getSeekPoints(int64_t timeUs) = 0;
};

/**
 * A {@link SeekMap} that does not support seeking.
 */
class UnSeekable final : public SeekMap {
public:
  explicit UnSeekable(const int64_t durationUs) noexcept
      : UnSeekable(durationUs, 0) {
  }

  explicit UnSeekable(const int64_t durationUs, const int64_t startPosition) noexcept
      : durationUs(durationUs),
        startSeekPoints(SeekPoint(0, startPosition)) {
  }

  [[nodiscard]] bool isSeekable() const override {
    return false;
  }

  [[nodiscard]] int64_t getDurationUs() const override {
    return durationUs;
  }

  SeekPoints getSeekPoints(int64_t timeUs) override {
    return startSeekPoints;
  }

private:
  int64_t durationUs;
  SeekPoints startSeekPoints;
};

} // namespace coral

#endif //CORAL_CODEC_TYPES_H
