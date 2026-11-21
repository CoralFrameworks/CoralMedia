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

#ifndef CORAL_FRAMEWORK_VIDEO_CODEC_FACTORY_H
#define CORAL_FRAMEWORK_VIDEO_CODEC_FACTORY_H

#include <codec/MediaCodecFactory.h>

#include <memory>

namespace coral {

class FrameworkVideoCodecFactory final : public VideoCodecFactory {
public:
  FrameworkVideoCodecFactory();

  ~FrameworkVideoCodecFactory() override;

  void setSoftwareFactory(std::unique_ptr<VideoCodecFactory> factory);

  bool isSupportedCodec(VideoCodecType codecType, bool decoder) override;

  std::unique_ptr<VideoDecoder>
  createVideoDecoder(VideoCodecType codecType, CodecManufacturer manufacturer) override;

  std::unique_ptr<VideoEncoder>
  createVideoEncoder(VideoCodecType codecType, CodecManufacturer manufacturer,
    const std::shared_ptr<SurfaceHolder>& surfaceHolder) override;

private:
  std::unique_ptr<VideoCodecFactory> builtinFactory;
  std::unique_ptr<VideoCodecFactory> softwareFactory;
};

} // namespace coral

#endif //CORAL_FRAMEWORK_VIDEO_CODEC_FACTORY_H
