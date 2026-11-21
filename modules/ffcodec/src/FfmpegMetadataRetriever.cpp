/*
 * Copyright (C) 2023 The Coral Project
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

#include <ffcodec/FfmpegMetadataRetriever.h>

#include <media/Types.h>

#include <minilog/log.h>

namespace coral {

std::unique_ptr<FfmpegMetadataRetriever>
FfmpegMetadataRetriever::sniff(const std::string& url) {
  av_register_all();
  avformat_network_init();
  AVFormatContext* formatContext = nullptr;
  auto ret = avformat_open_input(&formatContext, url.c_str(), nullptr, nullptr);
  if (ret != 0) {
    MINILOG_ERROR("Failed to prepare AVFormatContext: {}, error: {}", url, av_err2str(ret));
    avformat_close_input(&formatContext);
    return nullptr;
  }
  return std::make_unique<FfmpegMetadataRetriever>(formatContext);
}

FfmpegMetadataRetriever::FfmpegMetadataRetriever(
    AVFormatContext* formatContext) noexcept
    : pFormatContext(formatContext), needsFindStreamInfo(true) {}

void FfmpegMetadataRetriever::cancelLoading() {
  if (pFormatContext != nullptr) {
    if (pFormatContext->pb) {
      av_freep(&pFormatContext->pb->buffer);
      avio_closep(&pFormatContext->pb);
    }
    avformat_close_input(&pFormatContext);
    pFormatContext = nullptr;
  }
}

int FfmpegMetadataRetriever::getTrackCount() {
  if (pFormatContext != nullptr) {
    return (int)pFormatContext->nb_streams;
  }
  return Definitions::kInvalidValue;
}

std::shared_ptr<MediaFormat>
FfmpegMetadataRetriever::getTrackFormat(int trackIndex) {
  if (trackIndex >= getTrackCount()) {
    return nullptr;
  }

  if (needsFindStreamInfo) {
    needsFindStreamInfo = false;
    auto ret = avformat_find_stream_info(pFormatContext, nullptr);
    if (ret < 0) {
      MINILOG_ERROR("Failed to call avformat_find_stream_info: {}", av_err2str(ret));
      avformat_close_input(&pFormatContext);
      return nullptr;
    }
  }

  std::shared_ptr<MediaFormat> format = nullptr;
  auto stream = pFormatContext->streams[trackIndex];
  auto codecpar = stream->codecpar;

  auto mime = convertToMime(codecpar->codec_id);
  if (mime.empty()) {
    return format;
  }

  if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
    format = MediaFormat::CreateAudioFormat(mime, codecpar->sample_rate, codecpar->channels);
    if (codecpar->frame_size > 0) {
      auto inputSize = av_samples_get_buffer_size(
          nullptr, codecpar->channels, codecpar->frame_size, (AVSampleFormat)codecpar->format, 1);
      format->setMaxInputSize(inputSize);
    }
    if (stream->nb_frames > 0) {
      format->setFrameCount((int)stream->nb_frames);
    }
    if (codecpar->profile > 0) {
      format->setProfile(codecpar->profile);
    }
    if (codecpar->level > 0) {
      format->setLevel(codecpar->level);
    }
    auto sampleFormat = (AVSampleFormat)codecpar->format;
    switch (sampleFormat) {
      case AV_SAMPLE_FMT_S16:
      case AV_SAMPLE_FMT_S16P: {
        format->setSampleFormat(AudioSampleFormat::kInt16);
        break;
      }
      case AV_SAMPLE_FMT_S32:
      case AV_SAMPLE_FMT_S32P: {
        format->setSampleFormat(AudioSampleFormat::kInt32);
        break;
      }
      case AV_SAMPLE_FMT_FLT:
      case AV_SAMPLE_FMT_FLTP: {
        format->setSampleFormat(AudioSampleFormat::kFloat);
        break;
      }
      default: {
        break;
      }
    }
  }
  if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
    format = MediaFormat::CreateVideoFormat(mime, codecpar->width, codecpar->height);
    if (stream->nb_frames > 0) {
      format->setFrameCount(stream->nb_frames);
    }
    if (codecpar->profile > 0) {
      format->setProfile(codecpar->profile);
    }
    if (codecpar->level > 0) {
      format->setLevel(codecpar->level);
    }

    int rotateDegree = 0;
    if (stream->metadata != nullptr) {
      auto entry =
          av_dict_get(stream->metadata, "rotate", nullptr, AV_DICT_MATCH_CASE);
      if (entry && entry->value) {
        rotateDegree = atoi(entry->value);
      }
    }
    format->setRotation(rotateDegree);
    auto frameRate = (int)av_q2d(av_guess_frame_rate(pFormatContext, stream, nullptr));
    if (frameRate <= 0) {
      frameRate = Definitions::DEFAULT_FRAME_RATE;
    }
    format->setFrameRate(frameRate);
  }

  if (format != nullptr) {
    if (codecpar->bit_rate > 0) {
      format->setAverageBitrate((int)codecpar->bit_rate);
    }
    if (stream->duration != AV_NOPTS_VALUE) {
      auto const durationUs = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
      format->setDurationUs(durationUs);
    }
    format->setTrackId(stream->id);
  }

  return format;
}

std::string FfmpegMetadataRetriever::convertToMime(AVCodecID codecId) {
  switch (codecId) {
    case AV_CODEC_ID_MPEG2VIDEO: {
      return MimeTypes::VIDEO_MPEG2;
    }
    case AV_CODEC_ID_H263: {
      return MimeTypes::VIDEO_H263;
    }
    case AV_CODEC_ID_MPEG4: {
      return MimeTypes::VIDEO_MP4;
    }
    case AV_CODEC_ID_H264: {
      return MimeTypes::VIDEO_H264;
    }
    case AV_CODEC_ID_VP8: {
      return MimeTypes::VIDEO_VP8;
    }
    case AV_CODEC_ID_VP9: {
      return MimeTypes::VIDEO_VP9;
    }
    case AV_CODEC_ID_HEVC: {
      return MimeTypes::VIDEO_H265;
    }
    case AV_CODEC_ID_AV1: {
      return MimeTypes::VIDEO_AV1;
    }

    case AV_CODEC_ID_PCM_MULAW: {
      return MimeTypes::AUDIO_G711_MLAW;
    }
    case AV_CODEC_ID_PCM_ALAW: {
      return MimeTypes::AUDIO_G711_ALAW;
    }
    case AV_CODEC_ID_AMR_NB: {
      return MimeTypes::AUDIO_AMR_NB;
    }
    case AV_CODEC_ID_AMR_WB: {
      return MimeTypes::AUDIO_AMR_WB;
    }
    case AV_CODEC_ID_MP3: {
      return MimeTypes::AUDIO_MPEG;
    }
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM: {
      return MimeTypes::AUDIO_AAC;
    }
    case AV_CODEC_ID_AC3: {
      return MimeTypes::AUDIO_AC3;
    }
    case AV_CODEC_ID_DTS: {
      return MimeTypes::AUDIO_DTS;
    }
    case AV_CODEC_ID_VORBIS: {
      return MimeTypes::AUDIO_VORBIS;
    }
    case AV_CODEC_ID_FLAC: {
      return MimeTypes::AUDIO_FLAC;
    }
    case AV_CODEC_ID_ALAC: {
      return MimeTypes::AUDIO_ALAC;
    }
    case AV_CODEC_ID_EAC3: {
      return MimeTypes::AUDIO_E_AC3;
    }
    case AV_CODEC_ID_TRUEHD: {
      return MimeTypes::AUDIO_TRUEHD;
    }
    case AV_CODEC_ID_OPUS: {
      return MimeTypes::AUDIO_OPUS;
    }
    default: {
      return "";
    }
  }
}

} // namespace coral
