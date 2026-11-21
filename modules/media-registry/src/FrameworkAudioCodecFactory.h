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

#ifndef CORAL_FRAMEWORK_AUDIO_CODEC_FACTORY_H
#define CORAL_FRAMEWORK_AUDIO_CODEC_FACTORY_H

#include <codec/MediaCodecFactory.h>

#include <memory>

namespace coral {

class FrameworkAudioCodecFactory final : public AudioCodecFactory {
public:
  FrameworkAudioCodecFactory();

  ~FrameworkAudioCodecFactory() override;

  void setSoftwareCodecFactory(std::unique_ptr<AudioCodecFactory> factory);

  bool isSupportedCodec(AudioFormatType codecType, bool decoder) override;

  std::unique_ptr<AudioDecoder> createDecoder(const std::string& mimeType, bool outputFloat) override;

  std::unique_ptr<AudioEncoder> createEncoder(AudioFormatType codecType, CodecManufacturer manufacturer) override;

private:
  std::unique_ptr<AudioCodecFactory> builtinFactory;
  std::unique_ptr<AudioCodecFactory> softwareFactory;
};

} // namespace coral

#endif //CORAL_FRAMEWORK_AUDIO_CODEC_FACTORY_H
