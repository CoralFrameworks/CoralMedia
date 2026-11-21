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

#ifndef CORAL_MEDIA_TIME_H
#define CORAL_MEDIA_TIME_H

#include <media/Definitions.h>

#include <cmath>
#include <string>
#include <utility>

namespace coral {

struct AVTime {
  typedef enum {
    kTimeFlagsValid = (1 << 0),
    kTimeFlagsHasBeenRounded = (1 << 1),
    kTimeFlagsPositiveInfinity = (1 << 2),
    kTimeFlagsNegativeInfinity = (1 << 3),
    kTimeFlagsIndefinite = (1 << 4),
    kTimeFlagsImpliedValueFlagsMask =
    kTimeFlagsPositiveInfinity | kTimeFlagsNegativeInfinity | kTimeFlagsIndefinite
  } Flags;

  //!< time value
  int64_t value;
  //!< timescale, usually is the sample rate, default value is 44100
  uint32_t scale;
  //!< time's flags
  Flags flags;

  AVTime() : AVTime(Definitions::kUnspecified, Definitions::kMicrosPerSecond, kTimeFlagsValid) {
  }

  explicit AVTime(const int64_t value, const uint32_t scale) noexcept
    : AVTime(value, scale, kTimeFlagsValid) {
  }

  explicit AVTime(const int64_t value, const uint32_t scale, const Flags flags) noexcept
    : value(value),
      scale(scale),
      flags(flags) {
  }

  explicit AVTime(const std::pair<int64_t, uint32_t> other) noexcept
    : value(other.first),
      scale(other.second),
      flags(kTimeFlagsValid) {
  }

  AVTime(const AVTime& other) = default;
  AVTime& operator=(const AVTime& other) = default;

  ~AVTime() = default;

  void update(const int64_t newValue, const uint32_t newScale) {
    value = newValue;
    scale = newScale;
    flags = kTimeFlagsValid;
  }

  /**
   * Returns the time in microseconds
   */
  [[nodiscard]] int64_t timeUs() const {
    if (!isNumeric()) {
      return Definitions::kUnspecified;
    }
    return static_cast<int64_t>(std::round(value * Definitions::kMicrosPerSecond / scale));
  }

  /**
   * Returns the time in milliseconds
   */
  [[nodiscard]] int64_t timeMs() const {
    if (!isNumeric()) {
      return Definitions::kUnspecified;
    }
    return static_cast<int64_t>(std::round(value * Definitions::kMillisPerSecond / scale));
  }

  /**
   * Returns whether a Time is valid.
   */
  [[nodiscard]] bool isValid() const { return (flags & kTimeFlagsValid) != 0; }

  /**
   * Returns whether the time is invalid
   */
  [[nodiscard]] bool isInvalid() const { return !isValid(); }

  /**
   * Returns whether a Time is positive infinity.
   */
  [[nodiscard]] bool isPositiveInfinity() const {
    return isValid() && ((flags & kTimeFlagsPositiveInfinity) != 0);
  }

  /**
   * Returns whether the time is negative infinity.
   */
  [[nodiscard]] bool isNegativeInfinity() const {
    return isValid() && ((flags & kTimeFlagsNegativeInfinity) != 0);
  }

  /**
   * Returns whether the time is indefinite.
   */
  [[nodiscard]] bool isIndefinite() const {
    return isValid() && ((flags & kTimeFlagsIndefinite) != 0);
  }

  /**
   * Returns whether the time is numeric.
   * Returns false if the Time is invalid, indefinite or +/- infinity.
   * Returns true otherwise.
   */
  [[nodiscard]] bool isNumeric() const {
    return ((flags & (kTimeFlagsValid | kTimeFlagsImpliedValueFlagsMask)) == kTimeFlagsValid);
  }

  /**
   * Returns whether the time has been rounded.
   */
  [[nodiscard]] bool hasBeenRounded() const {
    return isNumeric() && ((flags & kTimeFlagsHasBeenRounded) != 0);
  }

  /**
   * Returns whether the time is zero
   */
  [[nodiscard]] bool isZero() const {
    return isNumeric() && value == Definitions::kUnspecified;
  }

  /**
   * Converts to second
   */
  [[nodiscard]] int64_t seconds() const {
    if (!isNumeric()) {
      return Definitions::kUnspecified;
    }
    return static_cast<int64_t>(std::round(value / scale));
  }

  /**
   * Converts to a new timescale
   */
  AVTime& convertScale(const int32_t newTimeScale) {
    if (scale == Definitions::kUnspecified || newTimeScale == Definitions::kUnspecified) {
      scale = Definitions::kUnspecified;
      flags = kTimeFlagsIndefinite;
    } else {
      value = static_cast<int64_t>(std::round(value * newTimeScale / scale));
      scale = newTimeScale;
    }
    return *this;
  }

  friend bool operator==(const AVTime& lhs, const AVTime& rhs) noexcept {
    return (lhs.value == rhs.value && lhs.scale == rhs.scale && lhs.flags == rhs.flags)
           || (lhs.isNumeric() && rhs.isNumeric() && lhs.timeUs() == rhs.timeUs());
  }

  friend bool operator!=(const AVTime& lhs, const AVTime& rhs) noexcept {
    return !(lhs == rhs);
  }

  friend bool operator<(const AVTime& lhs, const AVTime& rhs) noexcept {
    return (!lhs.isNumeric() && lhs.flags < rhs.flags) ||
           (lhs.isNumeric() && lhs.scale == rhs.scale && lhs.value < rhs.value) ||
           (lhs.isNumeric() && rhs.isNumeric() && lhs.timeUs() < rhs.timeUs());
  }

  friend bool operator<=(const AVTime& lhs, const AVTime& rhs) noexcept {
    return !(rhs < lhs);
  }

  friend bool operator>(const AVTime& lhs, const AVTime& rhs) noexcept {
    return rhs < lhs;
  }

  friend bool operator>=(const AVTime& lhs, const AVTime& rhs) noexcept {
    return !(lhs < rhs);
  }

  friend AVTime operator+(const AVTime& lhs, const AVTime& rhs) noexcept {
    if (lhs.isNumeric() && rhs.isNumeric()) {
      if (lhs.scale == rhs.scale) {
        return AVTime(lhs.value + rhs.value, lhs.scale);
      } else {
        auto newTimeUs = static_cast<int64_t>(std::round(lhs.timeUs() + rhs.timeUs()));
        auto time = AVTime(newTimeUs, Definitions::kMicrosPerSecond);
        time = time.convertScale(std::max(lhs.scale, rhs.scale));
        return time;
      }
    }
    // if any flag time, return the largest flag's time
    if (!lhs.isNumeric() && !rhs.isNumeric()) {
      return (lhs.flags > rhs.flags) ? lhs : rhs;
    } else if (lhs.isNumeric()) {
      return rhs;
    } else {
      return lhs;
    }
  }

  friend AVTime operator+(const AVTime& time, const int64_t newValue) noexcept {
    return AVTime(static_cast<int64_t>(std::round(time.value + newValue)), time.scale);
  }

  friend AVTime operator-(const AVTime& lhs, const AVTime& rhs) noexcept {
    if (lhs.isNumeric() && rhs.isNumeric()) {
      if (lhs.scale == rhs.scale) {
        return AVTime(static_cast<int64_t>(std::round(lhs.value - rhs.value)), lhs.scale);
      } else {
        return AVTime(lhs.timeUs() - rhs.timeUs(), Definitions::kMicrosPerSecond);
      }
    }
    return AVTime(Definitions::kUnspecified, Definitions::kUnspecified, Flags::kTimeFlagsIndefinite);
  }

  friend AVTime operator*(const AVTime& lhs, const AVTime& rhs) noexcept {
    if (lhs.isNumeric() && rhs.isNumeric()) {
      if (lhs.scale == rhs.scale) {
        return AVTime(static_cast<int64_t>(std::round(lhs.value * rhs.value)), lhs.scale);
      } else {
        return AVTime(std::round(lhs.timeUs() - rhs.timeUs()), Definitions::kMicrosPerSecond);
      }
    }
    return AVTime(Definitions::kUnspecified, Definitions::kUnspecified);
  }

  [[nodiscard]] std::string toString() const;
};

class AVTimeUtils {
public:
  //!< Indicates a value that represents time zero.
  static const AVTime& getZero();

  //!< Indicates a value that represents an indefinite time.
  static const AVTime& getIndefinite();

  //!< Indicates a value that represents a positive infinity.
  static const AVTime& getPositiveInfinity();

  //!< Indicates a value that represents a negative infinity.
  static const AVTime& getNegativeInfinity();

  //!< a value that represents an invalid time.
  //!< All fields are 0, so you can calloc or fill with 0's to make lots of them.
  static const AVTime& getInvalid();

  static const AVTime& max(const AVTime& lhs, const AVTime& rhs) {
    return (lhs < rhs) ? rhs : lhs;
  }

  static const AVTime& min(const AVTime& lhs, const AVTime& rhs) {
    return (lhs < rhs) ? lhs : rhs;
  }

  static AVTime fromTimeUs(int64_t presentationTimeUs) {
    return AVTime(presentationTimeUs, Definitions::kMicrosPerSecond);
  }

private:
  AVTimeUtils() = default;
  ~AVTimeUtils() = default;
};

} // namespace coral

#endif // CORAL_MEDIA_TIME_H
