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

#include <media/TimeRange.h>

#include <string>

namespace coral {

const AVTime& AVTimeUtils::getZero() {
  static const auto kTimeZero =
      AVTime(Definitions::kUnspecified, Definitions::kMicrosPerSecond, AVTime::kTimeFlagsValid);
  return kTimeZero;
}

const AVTime& AVTimeUtils::getIndefinite() {
  static const auto kIndefinite =
      AVTime(Definitions::kUnspecified,
          Definitions::kUnspecified,
          (AVTime::Flags)(AVTime::kTimeFlagsValid | AVTime::kTimeFlagsIndefinite));
  return kIndefinite;
}

const AVTime& AVTimeUtils::getPositiveInfinity() {
  static const auto kPositiveInfinity =
      AVTime(Definitions::kUnspecified, Definitions::kUnspecified,
          (AVTime::Flags)(AVTime::kTimeFlagsValid | AVTime::kTimeFlagsPositiveInfinity));
  return kPositiveInfinity;
}

const AVTime& AVTimeUtils::getNegativeInfinity() {
  static const auto kNegativeInfinity =
      AVTime(Definitions::kUnspecified, Definitions::kUnspecified,
          (AVTime::Flags)(AVTime::kTimeFlagsValid | AVTime::kTimeFlagsNegativeInfinity));
  return kNegativeInfinity;
}

const AVTime& AVTimeUtils::getInvalid() {
  static const auto kTimeInvalid = AVTime(0, 0, (AVTime::Flags)0);
  return kTimeInvalid;
}

const AVTimeRange& AVTimeRangeUtils::getRangeZero() {
  static const auto kRangeZero = AVTimeRange(AVTimeUtils::getZero(), AVTimeUtils::getZero());
  return kRangeZero;
}

const AVTimeRange& AVTimeRangeUtils::getRangeInvalid() {
  static const auto kRangeInvalid = AVTimeRange(AVTimeUtils::getInvalid(), AVTimeUtils::getInvalid());
  return kRangeInvalid;
}

bool AVTimeRangeUtils::isRangeValid(const AVTimeRange& timeRange) {
  return !timeRange.getStart().isInvalid() &&
         !timeRange.getDuration().isInvalid();
}

AVTime AVTimeRangeUtils::getRangeEnd(const AVTimeRange& range) {
  return range.getStart() + range.getDuration();
}

AVTime AVTimeRangeUtils::mapTimeFromRangeToRange(const AVTime& time,
    const AVTimeRange& fromRange,
    const AVTimeRange& toRange) {
  if (time <= fromRange.getStart()) {
    return toRange.getStart();
  } else if (time >= getRangeEnd(fromRange)) {
    return AVTime(getRangeEnd(toRange));
  } else {
    auto fromDuration = time - fromRange.getStart();
    return toRange.getStart() + mapDurationFromRangeToRange(fromDuration, fromRange, toRange);
  }
}

AVTime AVTimeRangeUtils::mapDurationFromRangeToRange(const AVTime& duration,
    const AVTimeRange& fromRange,
    const AVTimeRange& toRange) {
  if (duration.isInvalid() || duration.isZero()) {
    return AVTimeUtils::getZero();
  } else if (duration >= fromRange.getDuration()) {
    return toRange.getDuration();
  } else {
    auto durationTimeUs = duration.timeUs();
    auto fromTimeUs = fromRange.getDurationUs();
    auto toTimeUs = toRange.getDurationUs();
    if (fromTimeUs <= 0) {
      return AVTimeUtils::getZero();
    }
    auto newDuration = (int64_t)std::round((double)durationTimeUs * toTimeUs / fromTimeUs);
    return AVTime(newDuration, Definitions::kMicrosPerSecond)
        .convertScale((int32_t)toRange.getDuration().scale);
  }
}

AVTimeRange AVTimeRangeUtils::getUnionRange(const AVTimeRange& range, const AVTimeRange& otherRange) {
  auto start = AVTimeUtils::min(range.getStart(), otherRange.getStart());
  auto end = AVTimeUtils::max(range.getEnd(), otherRange.getEnd());
  auto duration = end - start;
  return AVTimeRange(start, duration);
}

AVTimeRange AVTimeRangeUtils::getIntersectionRange(const AVTimeRange& range,
    const AVTimeRange& otherRange) {
  auto endTime = getRangeEnd(range);
  auto otherEndTime = getRangeEnd(otherRange);
  if (range.getStart() > otherEndTime || endTime < otherRange.getStart()) {
    return AVTimeRangeUtils::getRangeZero();
  }
  auto intersectionStart =
      AVTimeUtils::max(range.getStart(), otherRange.getStart());
  auto intersectionEnd = AVTimeUtils::min(endTime, otherEndTime);
  return AVTimeRange(intersectionStart, intersectionEnd - intersectionStart);
}

AVTime AVTimeRangeUtils::clampToRange(const AVTime& time, const AVTimeRange& range) {
  if (time < range.getStart()) {
    return range.getStart();
  } else {
    auto end = getRangeEnd(range);
    if (time > end) {
      return end;
    }
  }
  return time;
}

bool AVTimeRangeUtils::rangeContainsTime(const AVTimeRange& range, const AVTime& time) {
  return range.getStart() <= time && getRangeEnd(range) >= time;
}

bool AVTimeRangeUtils::rangeContainsTimeRange(const AVTimeRange& range, const AVTimeRange& otherRange) {
  return range.getStart() <= otherRange.getStart()
         && getRangeEnd(range) >= getRangeEnd(otherRange);
}

//------------------------------------------------------------------------------

const AVTimeMapping& AVTimeMapping::getInvalid() {
  static const auto kTimeMappingInvalid = AVTimeMapping(
      AVTimeRangeUtils::getRangeInvalid(), AVTimeRangeUtils::getRangeInvalid());
  return kTimeMappingInvalid;
}

float AVTimeMapping::getScale() const {
  if (AVTimeRangeUtils::isRangeValid(source) &&
      AVTimeRangeUtils::isRangeValid(target)) {
    auto sourceDuration = source.getDuration();
    auto targetDuration = target.getDuration();
    if (sourceDuration.value == targetDuration.value &&
        sourceDuration.scale == targetDuration.scale) {
      return 1.0f;
    } else {
      auto sourceDurationUs = (float)sourceDuration.timeUs();
      auto targetDurationUs = (float)targetDuration.timeUs();
      return sourceDurationUs / targetDurationUs;
    }
  }
  return 1.0f;
}

//------------------------------------------------------------------------------

std::string AVTime::toString() const {
  std::string str;
  if (isZero()) {
    str.append("kTimeZero");
  } else if (isInvalid()) {
    str.append("kTimeInvalid");
  } else if (isIndefinite()) {
    str.append("kTimeIndefinite");
  } else if (isPositiveInfinity()) {
    str.append("kTimePositiveInfinity");
  } else if (isNegativeInfinity()) {
    str.append("kTimeNegativeInfinity");
  } else {
    str.append("{ value=")
       .append(std::to_string(value))
       .append(", scale=")
       .append(std::to_string(scale))
       .append(", flags=")
       .append(std::to_string(flags))
       .append(" }");
  }
  return str;
}

std::string AVTimeRange::toString() const {
  std::string str;
  str.append("{ start=")
     .append(mStart.toString())
     .append(", duration=")
     .append(mDuration.toString())
     .append(" }");
  return str;
}

std::string AVTimeMapping::toString() const {
  std::string str;
  str.append("{ source=")
     .append(source.toString())
     .append(", target=")
     .append(target.toString())
     .append(" }");
  return str;
}

} // namespace coral
