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

#ifndef CORAL_CODEC_MEDIA_CODEC_FACTORY_H
#define CORAL_CODEC_MEDIA_CODEC_FACTORY_H

#include <codec/MediaCodec.h>
#include <codec/CodecTypes.h>

#include <memory>

namespace coral {

/**
 * The factory for creating AudioDecoder/AudioEncoder
 */
class AudioCodecFactory {
public:
  virtual ~AudioCodecFactory() = default;

  /**
   * Whether supports the AudioFormatType
   */
  virtual bool isSupportedCodec(AudioFormatType formatType, bool decoder) = 0;

  /**
   * Creates an AudioDecoder with the mimeType
   *
   * @param mimeType the mimeType
   * @param outputFloat Whether the AudioDecoder is output float PCM data.
   *
   * @return a newly AudioDecoder
   */
  virtual std::unique_ptr<AudioDecoder>
  createDecoder(const std::string& mimeType, bool outputFloat) = 0;

  /**
   * Creates an AudioEncoder
   *
   * @param formatType  The audio format type
   * @param manufacturer The codec manufacturer
   * @return a newly AudioEncoder, nullptr if failure.
   */
  virtual std::unique_ptr<AudioEncoder>
  createEncoder(AudioFormatType formatType, CodecManufacturer manufacturer) = 0;
};

/**
 * Creates a new factory that can create the built-in types of audio decoders.
 */
std::unique_ptr<AudioCodecFactory> CreateBuiltinAudioCodecFactory();

//----------------------------------------------------------------------------------------------------------------------

/**
 * The factory for creating VideoCodec
 */
class VideoCodecFactory {
public:
  virtual ~VideoCodecFactory() = default;

  /**
   * Whether supports the VideoCodecType
   */
  virtual bool isSupportedCodec(VideoCodecType formatType, bool decoder) = 0;

  /**
   * Creates a new VideoDecoder
   */
  virtual std::unique_ptr<VideoDecoder>
  createVideoDecoder(VideoCodecType formatType, CodecManufacturer manufacturer) = 0;

  /**
   * Creates a new VideoEncoder
   */
  virtual std::unique_ptr<VideoEncoder>
  createVideoEncoder(VideoCodecType formatType,
      CodecManufacturer manufacturer,
      const std::shared_ptr<SurfaceHolder> &surfaceHolder) = 0;
};

/**
 * Creates a new factory that can create the built-in types of VideoCodec
 */
std::unique_ptr<VideoCodecFactory> CreateBuiltinVideoCodecFactory();

} // namespace coral

#endif //CORAL_CODEC_MEDIA_CODEC_FACTORY_H
