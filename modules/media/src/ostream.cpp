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

#include <media/Definitions.h>
#include <media/Types.h>

using namespace coral;

template<>
const char* convertToText<MediaType>(const MediaType type) {
  switch (type) {
    case MediaType::kMediaTypeInvalid: {
      return "invalid";
    }
    case MediaType::kMediaTypeDefault: {
      return "default";
    }
    case MediaType::kMediaTypeAudio: {
      return "audio";
    }
    case MediaType::kMediaTypeVideo: {
      return "video";
    }
    case MediaType::kMediaTypeSubtitle: {
      return "subtitle";
    }
    case MediaType::kMediaTypeImage: {
      return "image";
    }
    case MediaType::kMediaTypeMetadata: {
      return "metadata";
    }
    default: {
      return "unknown";
    }
  }
}

template<>
const char* convertToText<MediaCharacteristic>(const MediaCharacteristic characteristic) {
  switch (characteristic) {
    case MediaCharacteristic::kMediaCharacteristicAudible: {
      return "MediaCharacteristicAudible";
    }
    case MediaCharacteristic::kMediaCharacteristicVisual: {
      return "MediaCharacteristicVisual";
    }
    case MediaCharacteristic::kMediaCharacteristicLegible: {
      return "MediaCharacteristicLegible";
    }
    case MediaCharacteristic::kMediaCharacteristicUnknown: {
      return "Unknown media characteristic";
    }
  }
}

template<>
const char* convertToText<MediaFileType>(const MediaFileType fileType) {
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC: {
      return "aac";
    }
    case MediaFileType::kMediaFileTypeMpeg4Video: {
      return "mp4";
    }
    case MediaFileType::kMediaFileTypeH263: {
      return "h263";
    }
    case MediaFileType::kMediaFileTypeH264: {
      return "h264";
    }
    case MediaFileType::kMediaFileTypeFlac: {
      return "flac";
    }
    case MediaFileType::kMediaFileTypeOpus: {
      return "opus";
    }
    case MediaFileType::kMediaFileTypeWav: {
      return "wav";
    }
    default:
      return "unknown";
  }
}

template<>
const char* convertToText<AudioFormatType>(AudioFormatType type) {
  switch (type) {
    case AudioFormatType::kAudioFormatTypeLinearPCM: {
      return "linearPCM";
    }
    case AudioFormatType::kAudioFormatTypeAC3: {
      return "ac-3";
    }
    case AudioFormatType::kAudioFormatTypeMpeg4AAC: {
      return "aac";
    }
    case AudioFormatType::kAudioFormatTypeALaw: {
      return "alaw";
    }
    case AudioFormatType::kAudioFormatTypeMLaw: {
      return "malw";
    }
    case AudioFormatType::kAudioFormatTypeAMR: {
      return "samr";
    }
    case AudioFormatType::kAudioFormatTypeAMR_WB: {
      return "sawb";
    }
    case AudioFormatType::kAudioFormatTypeFlac: {
      return "flac";
    }
    case AudioFormatType::kAudioFormatTypeOpus: {
      return "opus";
    }
    case AudioFormatType::kAudioFormatTypeVorbis: {
      return "vorbis";
    }
    default: {
      return "unknown";
    }
  }
}

template<>
const char* convertToText<AudioSampleFormat>(const AudioSampleFormat sampleFormat) {
  switch (sampleFormat) {
    case AudioSampleFormat::kInvalid: {
      return "Invalid";
    }
    case AudioSampleFormat::kDefault: {
      return "Unspecified";
    }
    case AudioSampleFormat::kInt16: {
      return "encoding_pcm_16bit";
    }
    case AudioSampleFormat::kFloat: {
      return "encoding_pcm_float";
    }
    case AudioSampleFormat::kInt24: {
      return "encoding_pcm_24bit";
    }
    case AudioSampleFormat::kInt32: {
      return "encoding_pcm_32bit";
    }
    default: {
      return "unknown";
    }
  }
}

template<>
const char* convertToText<VideoCodecType>(const VideoCodecType type) {
  switch (type) {
    case VideoCodecType::kVideoCodecTypeH263: {
      return "h263";
    }
    case VideoCodecType::kVideoCodecTypeH264: {
      return "h264";
    }
    case VideoCodecType::kVideoCodecTypeHevc: {
      return "hevc";
    }
    case VideoCodecType::kVideoCodecTypeAV1: {
      return "av01";
    }
    case VideoCodecType::kVideoCodecTypeMpeg2Video: {
      return "mpeg2video";
    }
    case VideoCodecType::kVideoCodecTypeMpeg4Video: {
      return "mpeg4video";
    }
    case VideoCodecType::kVideoCodecTypeVP8: {
      return "vp8";
    }
    case VideoCodecType::kVideoCodecTypeVP9: {
      return "vp9";
    }
    case VideoCodecType::kVideoCodecTypeDolbyVision: {
      return "dolby-vision";
    }
    default: {
      return "unknown";
    }
  }
}

template<>
const char* convertToText<PixelFormat>(const PixelFormat format) {
  switch (format) {
    case PixelFormat::kFormatInvalid: {
      return "invalid";
    }
    case PixelFormat::kFormatSurface: {
      return "surface";
    }
    case PixelFormat::kFormatI420: {
      return "i420";
    }
    case PixelFormat::kFormatNV12: {
      return "nv12";
    }
    case PixelFormat::kFormatRGBA: {
      return "rgba";
    }
    case PixelFormat::kFormatBGRA: {
      return "bgra";
    }
    case PixelFormat::kFormatGray: {
      return "gray";
    }
    case PixelFormat::kFormatP010: {
      return "p010";
    }
    default: {
      return "unknown";
    }
  }
}
