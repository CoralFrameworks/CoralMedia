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

#ifndef CORAL_MEDIA_DEFINITIONS_H
#define CORAL_MEDIA_DEFINITIONS_H

#include <cstdint>

/**
 * Converts the definitions to text string
 */
template<typename FromType>
const char* convertToText(FromType input);

namespace coral {

class Definitions {
public:
  /** Represents any attribute, property or value which has an invalid value. */
  static constexpr int kInvalidValue = -1;

  /** Represents any attribute, property or value which hasn't been specified.
   */
  static constexpr int32_t kUnspecified = 0;

  /** Represents an unset or unknown integer rate. */
  static constexpr int32_t kRateUnset = INT32_MIN + 1;

  /** The number of nanoseconds in a microsecond. 1,000. */
  static constexpr int64_t kNanosPerMicrosecond = 1000;

  /** The number of nanoseconds in a millisecond. 1,000,000. */
  static constexpr int64_t kNanosPerMillisecond = kNanosPerMicrosecond * 1000;

  /** The number of microsecond in a millisecond. 1,000. */
  static constexpr int64_t kMicrosPerMillisecond = 1000;

  /** The number of milliseconds in a second. 1,000. */
  static constexpr int64_t kMillisPerSecond = 1000;

  /** The number of nanoseconds in a second. 1,000,000,000. */
  static constexpr int64_t kNanosPerSecond = kNanosPerMillisecond * kMillisPerSecond;

  /** The number of microsecond in a second. 1,000,000. */
  static constexpr int64_t kMicrosPerSecond = kMicrosPerMillisecond * kMillisPerSecond;

  /**
   * Special constant representing an unset or unknown time or duration.
   * Suitable for use in any time base.
   */
  static constexpr int64_t kTimeUnset = INT64_MIN + 1;

  /**
   * Special constant representing a time corresponding to the end of a source.
   * Suitable for use in any time base.
   */
  static constexpr int64_t kTimeEndOfSource = INT64_MIN;

  /** Represents an unset or unknown index. */
  static constexpr int kIndexUnset = kInvalidValue;

  /** Represents an unset or unknown position. */
  static constexpr int kPositionUnset = kInvalidValue;

  /** Represents an unset or unknown length. */
  static constexpr int kLengthUnset = kInvalidValue;

  /**
   * The default audio sample rate
   */
  static constexpr int DEFAULT_SAMPLE_RATE = 48000;

  /**
   * The default audio channel count
   */
  static constexpr int DEFAULT_CHANNEL_COUNT = 2;

  /**
   * 4 msec at 48000 Hz
   */
  static constexpr int DEFAULT_FRAME_PER_BURST = 192;

  /**
   * The default audio bitrate
   */
  static constexpr int DEFAULT_AUDIO_BITRATE = 128000;

  /**
   * The default video frame rate
   */
  static constexpr int DEFAULT_FRAME_RATE = 30;

  /**
   * The invalid codec type for AudioFormatType or VideoCodecType
   */
  static constexpr uint32_t kCodecTypeInvalid = 0;

  /**
   * A return value for methods where nothing was read.
   */
  static constexpr int kResultNothingRead = -3;
  /**
   * A value for methods where a buffer was read.
   */
  static constexpr int kResultBufferRead = -4;
  /**
   * A value for methods where a format was read.
   */
  static constexpr int kResultFormatRead = -5;
};

} // namespace coral

#endif //CORAL_MEDIA_DEFINITIONS_H
