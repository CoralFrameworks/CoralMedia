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

#ifndef CORAL_MEDIA_UTILS_H
#define CORAL_MEDIA_UTILS_H

#include <media/Types.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coral {

class MediaUtils final {
public:
  /**
   * Returns whether {@code encoding} is one of the linear PCM encodings.
   *
   * @param sampleFormat The specified sample format
   * @return Whether the encoding is one of the PCM encodings.
   */
  static bool isEncodingLinearPcm(AudioSampleFormat sampleFormat);

  /**
   * Returns whether {@code encoding} is high resolution (&gt; 16-bit) PCM.
   *
   * @param sampleFormat  The specified sample format
   * @return          Whether the encoding is high resolution PCM.
   */
  static bool isEncodingHighResolutionPcm(AudioSampleFormat sampleFormat);

  /**
   * Returns the frame size for audio with {@code channelCount} channels in the specified encoding.
   *
   * @param sampleFormat The specified sample format
   * @param channelCount The channel count.
   * @return The size of one audio frame in bytes.
   */
  static uint32_t getBytesPerFrame(AudioSampleFormat sampleFormat, uint32_t channelCount);

  /**
   * Gets the required buffer size for the given audio parameters.
   */
  static int
  getPcmBufferSize(int* lineSize, int32_t channelCount, int nbSamples, AudioSampleFormat sampleFormat, int align);

  /**
   * Gets the duration of the specified samples in microseconds.
   * If the format is invalid, returns {@link Definitions::kTimeUnset}.
   *
   * @param sampleRate    The specified sample rate
   * @param channelCount  The specified channels
   * @param sampleFormat  The specified sample format
   * @param length        The specified length
   * @return              The duration of the specified samples in microseconds
   */
  static int64_t
  getPcmDurationUs(int32_t sampleRate, int32_t channelCount, AudioSampleFormat sampleFormat, size_t length);

  /**
   * Gets the length of the specified samples.
   * If the format is invalid, returns {@link Definitions::kInvalidValue}
   *
   * @param sampleRate    The specified sample rate
   * @param channelCount  The specified channels
   * @param sampleFormat  The specified sample format
   * @param durationUs    The specified duration in microseconds
   * @return the length of the duration or Definitions::kInvalidValue
   */
  static int64_t
  getAudioByteSize(int32_t sampleRate, int32_t channelCount, AudioSampleFormat sampleFormat, int64_t durationUs);

  /**
   * Get the number of bytes per sample. This is calculated using the sample
   * format. For example, a stream using 16-bit integer samples will have 2
   * bytes per sample.
   *
   * @return the number of bytes per sample.
   */
  static uint32_t getBytesPerSample(AudioSampleFormat sampleFormat);

  /**
   * Check if the encoding is planar.
   * @param sampleFormat  The specified sample format
   * @return  true if the encoding is planar, false if it is interleaved
   */
  static bool isEncodingPlanar(AudioSampleFormat sampleFormat);

  /**
   * Converts a sample bit depth to a corresponding PCM encoding constant.
   *
   * @param bitDepth The bit depth. Supported values are 16, 24 and 32.
   * @return The corresponding AudioSampleFormat.
   */
  static AudioSampleFormat getSampleFormat(int bitDepth);

  /**
   * Converts AudioFormatType to mime type
   */
  static std::string convertToMimeType(AudioFormatType type);

  /**
   * Converts VideoCodecType to mime type
   */
  static std::string convertToMimeType(VideoCodecType type);

  /**
   * Converts mime type to AudioFormatType
   */
  static AudioFormatType convertToAudioFormatType(const std::string& mimeType);

  /**
   * Gets the default audio format type of the media file type
   */
  static AudioFormatType getDefaultAudioFormatType(const MediaFileType& fileType);

  /**
   * Converts mime type to VideoCodecType
   */
  static VideoCodecType convertToVideoCodecType(const std::string& mimeType);

  /**
   * Gets the default video codec type of the media file type
   */
  static VideoCodecType getDefaultVideoCodecType(MediaFileType fileType);

  /**
   * Returns the available audio file types
   */
  static const std::vector<MediaFileType>& audioAvailableFileTypes();

private:
  MediaUtils() = default;
  ~MediaUtils() = default;

  static const std::map<std::string, AudioFormatType>& availableAudioFormatTypes();

  static const std::map<std::string, VideoCodecType>& availableVideoCodecTypes();
};

} // namespace coral

#endif //CORAL_MEDIA_UTILS_H
