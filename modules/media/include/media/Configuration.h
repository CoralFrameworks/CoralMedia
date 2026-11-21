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

#ifndef CORAL_MEDIA_CONFIGURATION_H
#define CORAL_MEDIA_CONFIGURATION_H

#include <media/Definitions.h>
#include <media/Types.h>

#include <ostream>
#include <sstream>
#include <string>

namespace coral {

/**
 * The configuration of the audio sample buffer.
 */
struct AudioConfiguration {
  //!< the sample rate
  uint64_t sampleRate;
  //!< the channel count
  uint32_t channelCount;
  //!< The audio sample format
  AudioSampleFormat sampleFormat;

  AudioConfiguration()
      : sampleRate(Definitions::kUnspecified),
        channelCount(Definitions::kUnspecified),
        sampleFormat(AudioSampleFormat::kInvalid) {
  }

  constexpr AudioConfiguration(const uint64_t sampleRate, const uint32_t channelCount)
      : AudioConfiguration(sampleRate, channelCount, AudioSampleFormat::kInt16) {
  }

  constexpr AudioConfiguration(const uint64_t sampleRate,
      const uint32_t channelCount,
      const AudioSampleFormat format)
      : sampleRate(sampleRate),
        channelCount(channelCount),
        sampleFormat(format) {
  }

  [[nodiscard]] bool isInvalid() const {
    return sampleRate == 0 ||
        channelCount == 0 ||
        sampleFormat == AudioSampleFormat::kInvalid ||
        sampleFormat == AudioSampleFormat::kDefault;
  }

  friend bool operator==(const AudioConfiguration &lhs, const AudioConfiguration &rhs) {
    return lhs.sampleRate == rhs.sampleRate &&
        lhs.channelCount == rhs.channelCount &&
        lhs.sampleFormat == rhs.sampleFormat;
  }

  friend bool operator!=(const AudioConfiguration &lhs, const AudioConfiguration &rhs) {
    return !(lhs == rhs);
  }

  [[nodiscard]] std::string toString() const {
    std::stringstream out;
    out << "{"
        << "sampleRate=" << sampleRate
        << ", channelCount=" << channelCount
        << ", format=" << convertToText(sampleFormat)
        << "}";
    return out.str();
  }
};

/**
 * The configuration of the video frame.
 */
struct VideoConfiguration {
  //!< the width of video frame
  uint32_t width;
  //!< the height of video frame
  uint32_t height;
  //!< the rotation degree of the video frame
  int32_t rotationDegree;
  //!< the video frame rate
  uint32_t frameRate;
  //!< the video pixel format
  PixelFormat pixelFormat;

  VideoConfiguration()
      : width(Definitions::kUnspecified),
        height(Definitions::kUnspecified),
        rotationDegree(Definitions::kUnspecified),
        frameRate(Definitions::kUnspecified),
        pixelFormat(PixelFormat::kFormatInvalid) {
  }

  constexpr VideoConfiguration(const uint32_t width, const uint32_t height) noexcept
      : VideoConfiguration(width, height, Definitions::kUnspecified) {
  }

  constexpr VideoConfiguration(const uint32_t width, const uint32_t height, const uint32_t frameRate) noexcept
      : VideoConfiguration(width, height, frameRate, PixelFormat::kFormatInvalid) {
  }

  constexpr VideoConfiguration(const uint32_t width,
      const uint32_t height,
      const uint32_t frameRate,
      const PixelFormat pixelFormat) noexcept
      : VideoConfiguration(width, height, Definitions::kUnspecified, frameRate, pixelFormat) {
  }

  constexpr VideoConfiguration(const uint32_t width,
      const uint32_t height,
      const int32_t rotationDegree,
      const uint32_t frameRate,
      const PixelFormat pixelFormat) noexcept
      : width(width),
        height(height),
        rotationDegree(rotationDegree),
        frameRate(frameRate),
        pixelFormat(pixelFormat) {
  }

  [[nodiscard]] std::string toString() const {
    std::stringstream out;
    out << "{"
        << "width=" << width
        << ", height=" << height
        << ", rotationDegree=" << rotationDegree
        << ", frameRate=" << frameRate
        << ", pixelFormat=" << convertToText(pixelFormat)
        << "}";
    return out.str();
  }
};

} // namespace coral

inline std::ostream &operator<<(std::ostream &out, const coral::AudioConfiguration &configuration) {
  return out << configuration.toString();
}

inline std::ostream &operator<<(std::ostream &out, const coral::VideoConfiguration &configuration) {
  return out << configuration.toString();
}

#endif //CORAL_MEDIA_CONFIGURATION_H
