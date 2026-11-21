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

#ifndef CORAL_MEDIA_TIME_RANGE_H
#define CORAL_MEDIA_TIME_RANGE_H

#include <media/Time.h>

namespace coral {

class AVTimeRange {
private:
  //!< start time.
  AVTime mStart;
  //!< duration
  AVTime mDuration;

public:
  ~AVTimeRange() = default;

  AVTimeRange() : mStart(), mDuration() {}

  explicit AVTimeRange(const AVTime& start, const AVTime& duration) noexcept
      : mStart(start), mDuration(duration) {}

  AVTimeRange(const AVTimeRange& other) = default;
  AVTimeRange& operator=(const AVTimeRange& other) = default;

  bool isInvalid() const { return mStart.isInvalid() || mDuration.isInvalid(); }

  bool isNumeric() const { return mStart.isNumeric() && mDuration.isNumeric(); }

  /**
   * Returns the start time of the TimeRange.
   */
  [[nodiscard]] const AVTime& getStart() const { return mStart; }

  /**
   * Returns the start time of the TimeRange in microseconds.
   */
  [[nodiscard]] int64_t getStartUs() const {
    if (!mStart.isNumeric()) {
      return Definitions::kTimeUnset;
    }
    return mStart.timeUs();
  }

  /**
   * Returns the duration of the TimeRange.
   */
  [[nodiscard]] const AVTime& getDuration() const { return mDuration; }

  /**
   * Returns the duration of the TimeRange in microseconds.
   */
  [[nodiscard]] int64_t getDurationUs() const {
    if (!mDuration.isNumeric()) {
      return Definitions::kTimeUnset;
    }
    return mDuration.timeUs();
  }

  /**
   * Returns the end of the TimeRange.
   */
  AVTime getEnd() const { return mStart + mDuration; }

  /**
   * Returns the end of the TimeRange in microseconds.
   */
  [[nodiscard]] int64_t getEndUs() const {
    if (!mStart.isNumeric() || !mDuration.isNumeric()) {
      return Definitions::kTimeUnset;
    }
    return mStart.timeUs() + mDuration.timeUs();
  }

  /**
   * Indicates whether a time is contained within a time range.
   */
  bool contains(const AVTime& time) const noexcept {
    return mStart <= time && (mStart + mDuration) >= time;
  }

  /**
   * Indicates whether a time range is contained within a time range.
   */
  bool contains(const AVTimeRange& range) const noexcept {
    return mStart <= range.mStart && (mStart + mDuration) >= (range.mStart + range.mDuration);
  }

  friend bool operator==(const AVTimeRange& lhs, const AVTimeRange& rhs) noexcept {
    return lhs.mStart == rhs.mStart && lhs.mDuration == rhs.mDuration;
  }

  friend bool operator!=(const AVTimeRange& lhs, const AVTimeRange& rhs) noexcept {
    return !(lhs == rhs);
  }

  friend bool operator<=(const AVTimeRange& lhs, const AVTimeRange& rhs) noexcept {
    return lhs.mStart <= rhs.mStart;
  }

  friend bool operator>=(const AVTimeRange& lhs, const AVTimeRange& rhs) noexcept {
    return !(lhs <= rhs);
  }

  [[nodiscard]] std::string toString() const;
};

class AVTimeMapping {
private:
  //!< source time range. source.start is invalid for empty edits.
  AVTimeRange source;
  //!< track time range
  AVTimeRange target;

public:
  AVTimeMapping() : source(), target() {}

  explicit AVTimeMapping(const AVTimeRange& target) noexcept
      : AVTimeMapping(target, target) {}

  explicit AVTimeMapping(const AVTimeRange& source, const AVTimeRange& target) noexcept
      : source(source), target(target) {}

  AVTimeMapping(const AVTimeMapping& other) = default;

  AVTimeMapping& operator=(const AVTimeMapping& other) = default;

  [[nodiscard]] const AVTimeRange& getSource() const { return source; }

  [[nodiscard]] const AVTimeRange& getTarget() const { return target; }

  void updateSource(const AVTimeRange& newSource) { source = newSource; }

  void updateTarget(const AVTimeRange& newTarget) { target = newTarget; }

  [[nodiscard]] float getScale() const;

  [[nodiscard]] std::string toString() const;

  static const AVTimeMapping& getInvalid();
};

class AVTimeRangeUtils {
public:
  /**
   * Returns a zero range
   */
  static const AVTimeRange& getRangeZero();

  /**
   * Returns an invalid range
   */
  static const AVTimeRange& getRangeInvalid();

  static bool isRangeValid(const AVTimeRange& timeRange);

  /**
   * Returns the end of the TimeRange
   */
  static AVTime getRangeEnd(const AVTimeRange& range);

  /**
   * Translates a time through a mapping from TimeRange to TimeRange.
   */
  static AVTime mapTimeFromRangeToRange(const AVTime& time,
      const AVTimeRange& fromRange,
      const AVTimeRange& toRange);

  /**
   * Translates a duration through a mapping from TimeRange to TimeRange.
   */
  static AVTime mapDurationFromRangeToRange(const AVTime& duration,
      const AVTimeRange& fromRange,
      const AVTimeRange& toRange);

  /**
   * Returns the union of two TimeRanges.
   */
  static AVTimeRange getUnionRange(const AVTimeRange& range,
      const AVTimeRange& otherRange);

  /**
   * Returns the intersection of two TimeRanges.
   */
  static AVTimeRange getIntersectionRange(const AVTimeRange& range,
      const AVTimeRange& otherRange);

  /**
   * For a given Time and TimeRange, returns the nearest Time inside that time
   * range.
   */
  static AVTime clampToRange(const AVTime& time, const AVTimeRange& range);

  /**
   * Indicates whether a time is contained within a time range.
   */
  static bool rangeContainsTime(const AVTimeRange& range, const AVTime& time);

  /**
   * Indicates whether a time range is contained within a time range.
   */
  static bool rangeContainsTimeRange(const AVTimeRange& range,
      const AVTimeRange& otherRange);

private:
  AVTimeRangeUtils() = default;
  ~AVTimeRangeUtils() = default;
};

} // namespace coral

#endif // CORAL_MEDIA_TIME_RANGE_H
