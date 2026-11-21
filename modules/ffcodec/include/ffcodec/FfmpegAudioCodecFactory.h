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

#ifndef CORAL_CODEC_FFMPEG_AUDIO_CODEC_FACTORY_H
#define CORAL_CODEC_FFMPEG_AUDIO_CODEC_FACTORY_H

#include <codec/MediaCodecFactory.h>

namespace coral {

class FfmpegAudioCodecFactory final : public AudioCodecFactory {
public:
  FfmpegAudioCodecFactory();

  ~FfmpegAudioCodecFactory() override;

  bool isSupportedCodec(AudioFormatType codecType, bool decoder) override;

  std::unique_ptr<AudioDecoder>
  createDecoder(const std::string& mimeType, bool outputFloat) override;

  std::unique_ptr<AudioEncoder>
  createEncoder(AudioFormatType codecType, CodecManufacturer manufacturer) override;
};

} // namespace coral

#endif //CORAL_CODEC_FFMPEG_AUDIO_CODEC_FACTORY_H
