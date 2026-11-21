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

#include "private/ffcodec/FfmpegUtils.h"

#include <media/Types.h>
#include <media/Utilities.h>

namespace coral {

typedef struct CodecMimeInfo {
  AVCodecID codec;
  int32_t format;
  MediaType type;
  std::string name;
  std::string mimType;
} CodecMimeInfo;

static const CodecMimeInfo codec_mime_list[] = {
    //!< audio codec mime information
    {
        .codec = AV_CODEC_ID_AC3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAC3),
        .type = MediaType::kMediaTypeAudio,
        .name = "ac3",
        .mimType = MimeTypes::AUDIO_AC3,
    },
    {
        .codec = AV_CODEC_ID_EAC3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAC3),
        .type = MediaType::kMediaTypeAudio,
        .name = "eac3",
        .mimType = MimeTypes::AUDIO_E_AC3,
    },
    {
        .codec = AV_CODEC_ID_EAC3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAC3),
        .type = MediaType::kMediaTypeAudio,
        .name = "eac3",
        .mimType = MimeTypes::AUDIO_E_AC3_JOC,
    },
    {
        .codec = AV_CODEC_ID_MP3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMpeg3),
        .type = MediaType::kMediaTypeAudio,
        .name = "mp3",
        .mimType = MimeTypes::AUDIO_MPEG,
    },
    {
        .codec = AV_CODEC_ID_MP3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMpeg3),
        .type = MediaType::kMediaTypeAudio,
        .name = "mp3",
        .mimType = MimeTypes::AUDIO_MPEG_L1,
    },
    {
        .codec = AV_CODEC_ID_MP3,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMpeg3),
        .type = MediaType::kMediaTypeAudio,
        .name = "mp3",
        .mimType = MimeTypes::AUDIO_MPEG_L2,
    },
    {
        .codec = AV_CODEC_ID_AAC,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMpeg4AAC),
        .type = MediaType::kMediaTypeAudio,
        .name = "aac",
        .mimType = MimeTypes::AUDIO_AAC,
    },
    {
        .codec = AV_CODEC_ID_AAC_LATM,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMpeg4AAC),
        .type = MediaType::kMediaTypeAudio,
        .name = "aac",
        .mimType = MimeTypes::AUDIO_AAC
    },
    {
        .codec = AV_CODEC_ID_PCM_ALAW,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeALaw),
        .type = MediaType::kMediaTypeAudio,
        .name = "pcm_alaw",
        .mimType = MimeTypes::AUDIO_G711_ALAW,
    },
    {
        .codec = AV_CODEC_ID_PCM_MULAW,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeMLaw),
        .type = MediaType::kMediaTypeAudio,
        .name = "pcm_mulaw",
        .mimType = MimeTypes::AUDIO_G711_MLAW,
    },
    {
        .codec = AV_CODEC_ID_AMR_NB,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAMR),
        .type = MediaType::kMediaTypeAudio,
        .name = "amrnb",
        .mimType = MimeTypes::AUDIO_AMR_NB,
    },
    {
        .codec = AV_CODEC_ID_AMR_WB,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAMR_WB),
        .type = MediaType::kMediaTypeAudio,
        .name = "amrwb",
        .mimType = MimeTypes::AUDIO_AMR_WB,
    },
    {
        .codec = AV_CODEC_ID_FLAC,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeFlac),
        .type = MediaType::kMediaTypeAudio,
        .name = "flac",
        .mimType = MimeTypes::AUDIO_FLAC,
    },
    {
        .codec = AV_CODEC_ID_OPUS,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeOpus),
        .type = MediaType::kMediaTypeAudio,
        .name = "opus",
        .mimType = MimeTypes::AUDIO_OPUS,
    },
    {
        .codec = AV_CODEC_ID_VORBIS,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeOpus),
        .type = MediaType::kMediaTypeAudio,
        .name = "vorbis",
        .mimType = MimeTypes::AUDIO_VORBIS,
    },
    {
        .codec = AV_CODEC_ID_ALAC,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeAlac),
        .type = MediaType::kMediaTypeAudio,
        .name = "alac",
        .mimType = MimeTypes::AUDIO_ALAC,
    },
    {
        .codec = AV_CODEC_ID_TRUEHD,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeTrueHD),
        .type = MediaType::kMediaTypeAudio,
        .name = "truehd",
        .mimType = MimeTypes::AUDIO_TRUEHD,
    },
    {
        .codec = AV_CODEC_ID_DTS,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeDTS),
        .type = MediaType::kMediaTypeAudio,
        .name = "dca",
        .mimType = MimeTypes::AUDIO_DTS,
    },
    {
        .codec = AV_CODEC_ID_DTS,
        .format = static_cast<int32_t>(AudioFormatType::kAudioFormatTypeDTS),
        .type = MediaType::kMediaTypeAudio,
        .name = "dca",
        .mimType = MimeTypes::AUDIO_DTS_HD,
    },

    //!< video codec mime information
    {
        .codec = AV_CODEC_ID_H263,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeH263),
        .type = MediaType::kMediaTypeVideo,
        .name = "h263",
        .mimType = MimeTypes::VIDEO_H263,
    },
    {
        .codec = AV_CODEC_ID_H264,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeH264),
        .type = MediaType::kMediaTypeVideo,
        .name = "avc",
        .mimType = MimeTypes::VIDEO_H264,
    },
    {
        .codec = AV_CODEC_ID_HEVC,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeHevc),
        .type = MediaType::kMediaTypeVideo,
        .name = "hevc",
        .mimType = MimeTypes::VIDEO_H265,
    },
    {
        .codec = AV_CODEC_ID_AV1,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeAV1),
        .type = MediaType::kMediaTypeVideo,
        .name = "libaom-av1",
        .mimType = MimeTypes::VIDEO_AV1,
    },
    {
        .codec = AV_CODEC_ID_MPEG2VIDEO,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeMpeg2Video),
        .type = MediaType::kMediaTypeVideo,
        .name = "mpeg2video",
        .mimType = MimeTypes::VIDEO_MPEG2,
    },
    {
        .codec = AV_CODEC_ID_MPEG4,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeMpeg4Video),
        .type = MediaType::kMediaTypeVideo,
        .name = "mpeg4",
        .mimType = MimeTypes::VIDEO_MP4
    },
    {
        .codec = AV_CODEC_ID_VP8,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeVP8),
        .type = MediaType::kMediaTypeVideo,
        .name = "libvpx",
        .mimType = MimeTypes::VIDEO_VP8,
    },
    {
        .codec = AV_CODEC_ID_VP9,
        .format = static_cast<int32_t>(VideoCodecType::kVideoCodecTypeVP9),
        .type = MediaType::kMediaTypeVideo,
        .name = "libvpx-vp9",
        .mimType = MimeTypes::VIDEO_VP9,
    },
};

std::string FfmpegUtils::getCodecName(const std::string& mimeType) {
  for (auto const& info: codec_mime_list) {
    if (info.mimType == mimeType) {
      return info.name;
    }
  }
  // Unknown codec name
  return "";
}

std::string FfmpegUtils::convertToMime(AVCodecID codecId) {
  for (auto& info: codec_mime_list) {
    if (info.codec == codecId) {
      if (info.type == MediaType::kMediaTypeAudio) {
        return MediaUtils::convertToMimeType(static_cast<AudioFormatType>(info.format));
      }
      if (info.type == MediaType::kMediaTypeVideo) {
        return MediaUtils::convertToMimeType(static_cast<VideoCodecType>(info.format));
      }
    }
  }
  return {};
}

AVCodecID FfmpegUtils::convertToCodecID(const std::string& mime) {
  if (MimeTypes::isAudio(mime)) {
    auto audioFormatType = MediaUtils::convertToAudioFormatType(mime);
    if (audioFormatType != AudioFormatType::kAudioFormatTypeInvalid) {
      for (auto& info: codec_mime_list) {
        if (info.type == MediaType::kMediaTypeAudio &&
            static_cast<AudioFormatType>(info.format) == audioFormatType) {
          return info.codec;
        }
      }
    }
  } else if (MimeTypes::isVideo(mime)) {
    auto videoCodecType = MediaUtils::convertToVideoCodecType(mime);
    if (videoCodecType != VideoCodecType::kVideoCodecTypeInvalid) {
      for (auto& info: codec_mime_list) {
        if (info.type == MediaType::kMediaTypeVideo &&
            static_cast<VideoCodecType>(info.format) == videoCodecType) {
          return info.codec;
        }
      }
    }
  }

  return AV_CODEC_ID_NONE;
}

AVSampleFormat FfmpegUtils::convertToSampleFormat(AudioSampleFormat sampleFormat) {
  switch (sampleFormat) {
    case AudioSampleFormat::kInt16: {
      return AV_SAMPLE_FMT_S16;
    }
    case AudioSampleFormat::kInt32: {
      return AV_SAMPLE_FMT_S32;
    }
    case AudioSampleFormat::kFloat: {
      return AV_SAMPLE_FMT_FLT;
    }
    default: {
      return AV_SAMPLE_FMT_NONE;
    }
  }
}

AudioSampleFormat FfmpegUtils::convertToAudioSampleFormat(AVSampleFormat format) {
  switch (format) {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: {
      return AudioSampleFormat::kInt16;
    }
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: {
      return AudioSampleFormat::kInt32;
    }
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: {
      return AudioSampleFormat::kFloat;
    }
    default: {
      // We don't support other sample format
      return AudioSampleFormat::kInvalid;
    }
  }
}

PixelFormat FfmpegUtils::convertToPixelFormat(AVPixelFormat format) {
  switch (format) {
    case AV_PIX_FMT_NV12: {
      return PixelFormat::kFormatNV12;
    }
    case AV_PIX_FMT_YUV420P: {
      return PixelFormat::kFormatI420;
    }
    default: {
      return PixelFormat::kFormatInvalid;
    }
  }
}

AVPixelFormat FfmpegUtils::convertToAVPixelFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::kFormatNV12: {
      return AV_PIX_FMT_NV12;
    }
    case PixelFormat::kFormatI420: {
      return AV_PIX_FMT_YUV420P;
    }
    case PixelFormat::kFormatRGBA: {
      return AV_PIX_FMT_RGBA;
    }
    default: {
      return AV_PIX_FMT_NONE;
    }
  }
}

AudioFormatType FfmpegUtils::convertToAudioFormatType(AVCodecID codecId) {
  for (auto& info: codec_mime_list) {
    if (info.type == MediaType::kMediaTypeAudio && info.codec == codecId) {
      return static_cast<AudioFormatType>(info.format);
    }
  }
  return AudioFormatType::kAudioFormatTypeInvalid;
}

VideoCodecType FfmpegUtils::convertToVideoCodecType(AVCodecID codecId) {
  for (auto& info: codec_mime_list) {
    if (info.type == MediaType::kMediaTypeVideo && info.codec == codecId) {
      return static_cast<VideoCodecType>(info.format);
    }
  }
  return VideoCodecType::kVideoCodecTypeInvalid;
}

MediaType FfmpegUtils::getMediaType(AVMediaType mediaType) {
  if (mediaType == AVMEDIA_TYPE_AUDIO) {
    return MediaType::kMediaTypeAudio;
  }
  if (mediaType == AVMEDIA_TYPE_VIDEO) {
    return MediaType::kMediaTypeVideo;
  }
  return MediaType::kMediaTypeInvalid;
}

} // namespace coral
