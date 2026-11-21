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

#ifndef CORAL_CODEC_UTILITIES_H
#define CORAL_CODEC_UTILITIES_H

#include <codec/CodecTypes.h>
#include <media/Types.h>

#include <string>

namespace coral {

class CodecUtils final {
public:
  /**
   * Gets whether the audio decoder/encoder supports hardware codec.
   *
   * @param mimeType the mime type for audio decoder/encoder
   * @param encoder  if using for audio encoder
   * @return         CodecManufacturer
   */
  static CodecManufacturer getAudioCodecManufacturer(const std::string& mimeType, bool encoder);

  /**
   * Gets whether the audio decoder/encoder supports hardware codec.
   *
   * @param formatType the audio format type for audio decoder/encoder
   * @param encoder   if using for audio encoder
   * @return          CodecManufacturer
   */
  static CodecManufacturer getAudioCodecManufacturer(AudioFormatType formatType, bool encoder);

  /**
   * Gets whether the video decoder/encoder supports hardware codec.
   *
   * @param mimeType the mime type for video decoder/encoder
   * @param encoder  if using for video encoder
   * @return         CodecManufacturer
   */
  static CodecManufacturer getVideoCodecManufacturer(const std::string& mimeType, bool encoder);

private:
  CodecUtils() = default;
  ~CodecUtils() = default;
};

} // namespace coral

#endif //CORAL_CODEC_UTILITIES_H
