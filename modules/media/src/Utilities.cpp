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

#include <media/Utilities.h>

#include <media/Definitions.h>
#include <media/Types.h>

#include <climits>
#include <cstdint>
#include <numeric>
#include <sstream>

namespace coral {

/* error handling */
#if EDOM > 0
#define AUDIO_ERR(e)                                                           \
(-(e)) //!< Returns a negative error code from a POSIX error code, to return
//!< from library functions.
#define AUDIO_UNERR(e)                                                         \
(-(e)) //!< Returns a POSIX error code from a library function error return
//!< value.
#else
#define AUDIO_ERR(e) (e)
#define AUDIO_UNERR(e) (e)
#endif

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// -----------------------------------------------------------------------------

typedef struct SampleFormatInfo {
  char name[256];
  int bits;
  int planar;
  AudioSampleFormat format; //!< planar<->packed alternative form
} SampleFormatInfo;

static const SampleFormatInfo sample_format_info[] = {
  {
    .name = "sample_format_float",
    .bits = 32,
    .planar = 0,
    .format = AudioSampleFormat::kFloat
  },
  {
    .name = "sample_format_16bit",
    .bits = 16,
    .planar = 0,
    .format = AudioSampleFormat::kInt16
  },
  {
    .name = "sample_format_24bit",
    .bits = 24,
    .planar = 0,
    .format = AudioSampleFormat::kInt24
  },
  {
    .name = "sample_format_32bit",
    .bits = 32,
    .planar = 0,
    .format = AudioSampleFormat::kInt32
  },
};

bool MediaUtils::isEncodingLinearPcm(const AudioSampleFormat sampleFormat) {
  return sampleFormat == AudioSampleFormat::kInt16
         || sampleFormat == AudioSampleFormat::kInt24
         || sampleFormat == AudioSampleFormat::kInt32
         || sampleFormat == AudioSampleFormat::kFloat;
}

bool MediaUtils::isEncodingHighResolutionPcm(const AudioSampleFormat sampleFormat) {
  return sampleFormat == AudioSampleFormat::kInt24
         || sampleFormat == AudioSampleFormat::kInt32
         || sampleFormat == AudioSampleFormat::kFloat;
}

uint32_t MediaUtils::getBytesPerFrame(const AudioSampleFormat sampleFormat, const uint32_t channelCount) {
  return channelCount * getBytesPerSample(sampleFormat);
}

int MediaUtils::getPcmBufferSize(int* lineSize,
  const int32_t channelCount,
  int nbSamples,
  const AudioSampleFormat sampleFormat,
  int align) {

  int line_size;
  int sample_size = getBytesPerSample(sampleFormat);
  bool planar = isEncodingPlanar(sampleFormat);

  /* validate parameter ranges */
  if (!sample_size || nbSamples <= 0 || channelCount <= 0) {
    return AUDIO_ERR(EINVAL);
  }

  /* auto-select alignment if not specified */
  if (!align) {
    if (nbSamples > INT_MAX - 31) {
      return AUDIO_ERR(EINVAL);
    }
    align = 1;
    nbSamples = ALIGN(nbSamples, 32);
  }

  /* check for integer overflow */
  if (channelCount > INT_MAX / align ||
      (int64_t) channelCount * nbSamples > (INT_MAX - (align * channelCount)) / sample_size) {
    return AUDIO_ERR(EINVAL);
  }

  line_size = planar
                ? ALIGN(nbSamples * sample_size, align)
                : ALIGN(nbSamples * sample_size * channelCount, align);

  if (lineSize) {
    *lineSize = line_size;
  }
  return planar ? line_size * channelCount : line_size;
}

int64_t MediaUtils::getPcmDurationUs(const int32_t sampleRate,
  const int32_t channelCount,
  const AudioSampleFormat sampleFormat,
  const size_t length) {
  const auto frameSize = getBytesPerFrame(sampleFormat, channelCount);
  if (frameSize <= 0) {
    return Definitions::kTimeUnset;
  }
  return (int64_t) length * Definitions::kMicrosPerSecond / (frameSize * sampleRate);
}

int64_t MediaUtils::getAudioByteSize(const int32_t sampleRate,
  const int32_t channelCount,
  const AudioSampleFormat sampleFormat,
  const int64_t durationUs) {
  const auto audioSampleCount = durationUs * sampleRate / Definitions::kMicrosPerSecond;
  return getBytesPerFrame(sampleFormat, channelCount) * audioSampleCount;
}

uint32_t MediaUtils::getBytesPerSample(const AudioSampleFormat sampleFormat) {
  int32_t size = 0;
  switch (sampleFormat) {
    case AudioSampleFormat::kInt16: {
      size = sizeof(int16_t); // 2 bytes
      break;
    }
    case AudioSampleFormat::kFloat: {
      size = sizeof(float); // 4 bytes
      break;
    }
    case AudioSampleFormat::kInt24: {
      size = 3; // packed 24-bit data, 3 bytes
      break;
    }
    case AudioSampleFormat::kInt32: {
      size = sizeof(int32_t); // 4 bytes
      break;
    }
    default: {
      break;
    }
  }
  return size;
}

bool MediaUtils::isEncodingPlanar(const AudioSampleFormat sampleFormat) {
  for (const auto& info: sample_format_info) {
    if (info.format == sampleFormat) {
      return static_cast<bool>(sample_format_info->planar);
    }
  }
  return false;
}

AudioSampleFormat MediaUtils::getSampleFormat(const int bitDepth) {
  switch (bitDepth) {
    case 16: {
      return AudioSampleFormat::kInt16;
    }
    case 24: {
      return AudioSampleFormat::kInt24;
    }
    case 32: {
      return AudioSampleFormat::kInt32;
    }
    default: {
      return AudioSampleFormat::kInvalid;
    }
  }
}

std::string MediaUtils::convertToMimeType(const AudioFormatType type) {
  for (const auto& pair: availableAudioFormatTypes()) {
    if (pair.second == type) {
      return pair.first;
    }
  }
  return {};
}

std::string MediaUtils::convertToMimeType(const VideoCodecType type) {
  for (const auto& pair: availableVideoCodecTypes()) {
    if (pair.second == type) {
      return pair.first;
    }
  }
  return {};
}

AudioFormatType
MediaUtils::convertToAudioFormatType(const std::string& mimeType) {
  for (const auto& pair: availableAudioFormatTypes()) {
    if (pair.first == mimeType) {
      return pair.second;
    }
  }
  return AudioFormatType::kAudioFormatTypeInvalid;
}

AudioFormatType MediaUtils::getDefaultAudioFormatType(const MediaFileType& fileType) {
  AudioFormatType codecType;
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC:
    case MediaFileType::kMediaFileTypeMpeg4Video: {
      codecType = AudioFormatType::kAudioFormatTypeMpeg4AAC;
      break;
    }
    case MediaFileType::kMediaFileTypeFlac: {
      codecType = AudioFormatType::kAudioFormatTypeFlac;
      break;
    }
    case MediaFileType::kMediaFileTypeOpus: {
      codecType = AudioFormatType::kAudioFormatTypeOpus;
      break;
    }
    case MediaFileType::kMediaFileTypeWav: {
      codecType = AudioFormatType::kAudioFormatTypeLinearPCM;
      break;
    }
    case MediaFileType::kMediaFileTypeUnknown:
    case MediaFileType::kMediaFileTypeH263:
    case MediaFileType::kMediaFileTypeH264:
    default: {
      codecType = AudioFormatType::kAudioFormatTypeInvalid;
      break;
    }
  }
  return codecType;
}

VideoCodecType
MediaUtils::convertToVideoCodecType(const std::string& mimeType) {
  if (MimeTypes::getMediaType(mimeType) == MediaType::kMediaTypeVideo) {
    for (auto& pair: availableVideoCodecTypes()) {
      if (pair.first == mimeType) {
        return pair.second;
      }
    }
  }
  return VideoCodecType::kVideoCodecTypeInvalid;
}

VideoCodecType MediaUtils::getDefaultVideoCodecType(const MediaFileType fileType) {
  auto codecType = VideoCodecType::kVideoCodecTypeInvalid;
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4Video: {
      codecType = VideoCodecType::kVideoCodecTypeH264;
      break;
    }
    case MediaFileType::kMediaFileTypeH263: {
      codecType = VideoCodecType::kVideoCodecTypeH263;
      break;
    }
    case MediaFileType::kMediaFileTypeH264: {
      codecType = VideoCodecType::kVideoCodecTypeH264;
      break;
    }
    case MediaFileType::kMediaFileTypeUnknown:
    case MediaFileType::kMediaFileTypeMpeg4AAC:
    case MediaFileType::kMediaFileTypeFlac:
    case MediaFileType::kMediaFileTypeOpus:
    case MediaFileType::kMediaFileTypeWav:
    default: {
      break;
    }
  }
  return codecType;
}

const std::vector<MediaFileType>& MediaUtils::audioAvailableFileTypes() {
  static const std::vector<MediaFileType> sAudioFileTypes = {
    MediaFileType::kMediaFileTypeMpeg4AAC,
    MediaFileType::kMediaFileTypeFlac,
    MediaFileType::kMediaFileTypeOpus,
    MediaFileType::kMediaFileTypeWav
  };
  return sAudioFileTypes;
}

const std::map<std::string, AudioFormatType>& MediaUtils::availableAudioFormatTypes() {
  static const std::map<std::string, AudioFormatType> sAudioFormatTypes = {
    { MimeTypes::AUDIO_AC3, AudioFormatType::kAudioFormatTypeAC3 },
    { MimeTypes::AUDIO_G711_ALAW, AudioFormatType::kAudioFormatTypeALaw },
    { MimeTypes::AUDIO_G711_MLAW, AudioFormatType::kAudioFormatTypeMLaw },
    { MimeTypes::AUDIO_AMR_NB, AudioFormatType::kAudioFormatTypeAMR },
    { MimeTypes::AUDIO_AMR_WB, AudioFormatType::kAudioFormatTypeAMR_WB },
    { MimeTypes::AUDIO_FLAC, AudioFormatType::kAudioFormatTypeFlac },
    { MimeTypes::AUDIO_MPEG, AudioFormatType::kAudioFormatTypeMpeg3 },
    { MimeTypes::AUDIO_AAC, AudioFormatType::kAudioFormatTypeMpeg4AAC },
    { MimeTypes::AUDIO_OPUS, AudioFormatType::kAudioFormatTypeOpus },
    { MimeTypes::AUDIO_VORBIS, AudioFormatType::kAudioFormatTypeVorbis },
    { MimeTypes::AUDIO_RAW, AudioFormatType::kAudioFormatTypeLinearPCM },
  };
  return sAudioFormatTypes;
}

const std::map<std::string, VideoCodecType>& MediaUtils::availableVideoCodecTypes() {
  static const std::map<std::string, VideoCodecType> sVideoCodecTypes = {
    { MimeTypes::VIDEO_H263, VideoCodecType::kVideoCodecTypeH263 },
    { MimeTypes::VIDEO_H264, VideoCodecType::kVideoCodecTypeH264 },
    { MimeTypes::VIDEO_H265, VideoCodecType::kVideoCodecTypeHevc },
    { MimeTypes::VIDEO_AV1, VideoCodecType::kVideoCodecTypeAV1 },
    { MimeTypes::VIDEO_MPEG2, VideoCodecType::kVideoCodecTypeMpeg2Video },
    { MimeTypes::VIDEO_MP4, VideoCodecType::kVideoCodecTypeMpeg4Video },
    { MimeTypes::VIDEO_MP4V, VideoCodecType::kVideoCodecTypeMpeg4Video },
    { MimeTypes::VIDEO_MP42, VideoCodecType::kVideoCodecTypeMpeg4Video },
    { MimeTypes::VIDEO_MP43, VideoCodecType::kVideoCodecTypeMpeg4Video },
    { MimeTypes::VIDEO_VP8, VideoCodecType::kVideoCodecTypeVP8 },
    { MimeTypes::VIDEO_VP9, VideoCodecType::kVideoCodecTypeVP9 },
    { MimeTypes::VIDEO_DOLBY_VISION, VideoCodecType::kVideoCodecTypeDolbyVision },
  };
  return sVideoCodecTypes;
}



} // namespace coral
