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

#include <ffcodec/FfmpegExtractor.h>

#include <media/PacketBuffer.h>
#include <media/Utilities.h>
#include <media/Definitions.h>

#include <minilog/log.h>

#include <algorithm>

namespace coral {

std::shared_ptr<FfmpegExtractor> FfmpegExtractor::createWithUrl(const std::string &url) {
  avformat_network_init();
  AVFormatContext *formatContext = nullptr;

  auto ret = avformat_open_input(&formatContext, url.c_str(), nullptr, nullptr);
  if (ret != 0) {
    MINILOG_ERROR("Failed to prepare AVFormatContext:{}, error: {}", url, ffErrorString(ret));
    avformat_close_input(&formatContext);
    return {};
  }

  // Find local file stream information
  if (!isRealTime(formatContext)) {
    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
      MINILOG_ERROR("Failed to call avformat_find_stream_info: {}", ffErrorString(ret));
      avformat_close_input(&formatContext);
      return {};
    }
  }

  return std::make_shared<FfmpegExtractor>(formatContext);
}

std::shared_ptr<FfmpegExtractor> FfmpegExtractor::createForInput(std::string url) {
  // Nothing is opened here: the media is the one of the reader, which is given to the extractor when it
  // is asked to recognize the container. The extractor is built through its private constructor, which
  // a make_shared of this class cannot reach.
  return std::shared_ptr<FfmpegExtractor>(new FfmpegExtractor(std::move(url)));
}

int FfmpegExtractor::readPacket(void *opaque, uint8_t *buffer, const int size) {
  auto *extractor = static_cast<FfmpegExtractor *>(opaque);
  if (extractor == nullptr || extractor->mInput == nullptr || size <= 0) {
    return AVERROR(EINVAL);
  }
  const long read = extractor->mInput->read(buffer, 0, static_cast<size_t>(size));
  if (read == AVMediaExtractorInput::kEndOfInput) {
    // The end of the media is reported the way ffmpeg expects it, i.e. as the end of the file.
    return AVERROR_EOF;
  }
  return static_cast<int>(read);
}

int64_t FfmpegExtractor::seekInput(void *opaque, const int64_t offset, const int whence) {
  auto *extractor = static_cast<FfmpegExtractor *>(opaque);
  if (extractor == nullptr || extractor->mInput == nullptr) {
    return AVERROR(EINVAL);
  }
  if ((whence & AVSEEK_SIZE) != 0) {
    return extractor->mInput->getLength();
  }
  int64_t target = offset;
  if ((whence & ~AVSEEK_FORCE) == SEEK_CUR) {
    target += extractor->mInput->getPosition();
  } else if ((whence & ~AVSEEK_FORCE) == SEEK_END) {
    const long length = extractor->mInput->getLength();
    if (length < 0) {
      return AVERROR(EINVAL);
    }
    target += length;
  }
  if (target < 0) {
    return AVERROR(EINVAL);
  }
  if (!extractor->mInput->setPosition(static_cast<long>(target), true)) {
    return AVERROR(EIO);
  }
  return static_cast<int64_t>(extractor->mInput->getPosition());
}

FfmpegExtractor::FfmpegExtractor(AVFormatContext *formatContext) noexcept
    : pFormatContext(formatContext),
      pVideoStreamFilter(nullptr),
      pVideoStreamContext(nullptr),
      pExtractorOutput(nullptr),
      mNeedPrepareFormat(true),
      mHasAudioTrack(false),
      mHasVideoTrack(false),
      mReadingPositionUs(Definitions::kTimeUnset) {
  avformat_network_init();
  pCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

FfmpegExtractor::FfmpegExtractor(std::string url) noexcept
    : mInputUrl(std::move(url)),
      pVideoStreamFilter(nullptr),
      pVideoStreamContext(nullptr),
      pExtractorOutput(nullptr),
      mNeedPrepareFormat(true),
      mHasAudioTrack(false),
      mHasVideoTrack(false),
      mReadingPositionUs(Definitions::kTimeUnset) {
  avformat_network_init();
  pCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

FfmpegExtractor::~FfmpegExtractor() {
  destroy();
}

void FfmpegExtractor::destroy() {
  clear();
  pExtractorOutput = nullptr;
}

int FfmpegExtractor::sniff(AVExtractorInput &input) {
  if (pFormatContext != nullptr) {
    // The media was opened by the url this extractor was created with, so it reads itself and it
    // recognizes the container it was created for.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  // The media is the one of the reader: ffmpeg reads it through the io context of this extractor, which
  // is what makes the bytes it consumed the bytes the reader measures.
  if (!openFromInput(input)) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

bool FfmpegExtractor::openFromInput(AVExtractorInput &input) {
  if (pFormatContext != nullptr) {
    return true;
  }
  mInput = std::make_unique<AVMediaExtractorInput>(input);
  // The io context of ffmpeg reads through the input of the reader rather than through a file of its
  // own: the buffer belongs to this extractor, so it is freed with it.
  constexpr int kIoBufferSize = 32 * 1024;
  auto *ioBuffer = static_cast<uint8_t *>(av_malloc(kIoBufferSize));
  if (ioBuffer == nullptr) {
    MINILOG_ERROR("Failed to allocate the io buffer of the media of the reader");
    return false;
  }
  pIoContext = avio_alloc_context(ioBuffer, kIoBufferSize, 0, this, &FfmpegExtractor::readPacket,
      nullptr, &FfmpegExtractor::seekInput);
  if (pIoContext == nullptr) {
    av_freep(&ioBuffer);
    MINILOG_ERROR("Failed to create the io context of the media of the reader");
    return false;
  }

  AVFormatContext *formatContext = avformat_alloc_context();
  if (formatContext == nullptr) {
    MINILOG_ERROR("Failed to allocate the format context of the media of the reader");
    return false;
  }
  formatContext->pb = pIoContext;
  formatContext->flags |= AVFMT_FLAG_CUSTOM_IO;

  // The url is not the media here, the io context is, so ffmpeg is given no url to open. The context is
  // owned by this extractor only once it is open, because a failed open frees it itself.
  auto ret = avformat_open_input(&formatContext, nullptr, nullptr, nullptr);
  if (ret != 0) {
    MINILOG_ERROR("Failed to open the media of the reader: {}", ffErrorString(ret));
    return false;
  }
  pFormatContext = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>(formatContext);

  ret = avformat_find_stream_info(formatContext, nullptr);
  if (ret < 0) {
    MINILOG_ERROR("Failed to call avformat_find_stream_info: {}", ffErrorString(ret));
    pFormatContext.reset();
    return false;
  }
  return true;
}

void FfmpegExtractor::init(AVExtractorOutput &output) {
  pExtractorOutput = &output;
  prepareFormat();
}

AVMediaExtractor::PlatformResult FfmpegExtractor::read(PositionHolder &seekPosition) {
  static_cast<void>(seekPosition);
  int ret;
  if (pFormatContext == nullptr ||
      mReadingPositionUs == Definitions::kTimeEndOfSource) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  // prepare format first
  if (mNeedPrepareFormat) {
    prepareFormat();
  }

  auto result = PlatformResult::RESULT_CONTINUE;
  bool hasReadPacket = false;
  while (true) {
    auto packet = pCachePacket.get();
    av_packet_unref(packet);
    ret = av_read_frame(pFormatContext.get(), packet);
    if (ret < 0) {
      // write end of stream signal to track output
      if (ret == AVERROR_EOF) {
        result = PlatformResult::RESULT_END_OF_INPUT;
        mReadingPositionUs = Definitions::kTimeEndOfSource;
        notifyEndOfStream();
      } else {
        result = PlatformResult::RESULT_CONTINUE;
        MINILOG_ERROR("Failed to call av_read_frame: {}", ffErrorString(ret));
      }
      break;
    }

    mReadingPosition = packet->pos;
    // Whether the track can be write packet data
    for (auto &track : tracks) {
      if (track.isValid() && track.getTrackIndex() == packet->stream_index) {
        if (packet->size <= 0) {
          result = PlatformResult::RESULT_END_OF_INPUT;
        } else {
          auto stream = pFormatContext->streams[packet->stream_index];
          auto time_base = stream->time_base;
          if (stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
              stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            parseVideoBitStream(packet);
          }
          int64_t timeUs = av_rescale_q(packet->pts, time_base, AVRational{1, AV_TIME_BASE});
          track.enqueuePacketData(packet, timeUs);
          result = PlatformResult::RESULT_CONTINUE;
          if (mReadingPositionUs == Definitions::kTimeUnset) {
            mReadingPositionUs = timeUs;
          } else {
            mReadingPositionUs = std::max(mReadingPositionUs, timeUs);
          }
        }
        hasReadPacket = true;
        break;
      }
    }

    // Checks if we have read packet, break the while-looping
    if (hasReadPacket) {
      break;
    }
  }

  return result;
}

size_t FfmpegExtractor::getTrackCount() const {
  if (pFormatContext != nullptr) {
    return pFormatContext->nb_streams;
  }
  return 0;
}

void FfmpegExtractor::prepareFormat() {
  if (mNeedPrepareFormat && pExtractorOutput != nullptr) {
    auto trackCount = getTrackCount();
    for (int i = 0; i < trackCount; ++i) {
      auto stream = pFormatContext->streams[i];
      AVMediaType mediaType = stream->codecpar->codec_type;
      if (mediaType == AVMEDIA_TYPE_AUDIO || mediaType == AVMEDIA_TYPE_VIDEO) {
        auto type = mediaType == AVMEDIA_TYPE_AUDIO
            ? MediaType::kMediaTypeAudio
            : MediaType::kMediaTypeVideo;
        auto trackOutput = pExtractorOutput->track(pFormatContext->streams[i]->id, type,
            AVTrackOutput::emptyConsumer());
        if (trackOutput != nullptr) {
          tracks.emplace_back(stream->index, pFormatContext->streams[i]->id, type, std::move(trackOutput));
        } else {
          tracks.emplace_back(stream->index);
        }
        if (mediaType == AVMEDIA_TYPE_AUDIO) {
          mHasAudioTrack = true;
        } else {
          mHasVideoTrack = true;
        }
      } else {
        // Unavailable track
        tracks.emplace_back(stream->index);
      }
    }

    // prepare formats
    if (!tracks.empty()) {
      for (auto &track : tracks) {
        if (track.isValid()) {
          track.changeFormat(getTrackFormat(track.getTrackIndex()));
        }
      }
    }
    mNeedPrepareFormat = false;
    pExtractorOutput->endTracks();
    // The extractor is the seek map of the media it reads, and it is owned by a shared pointer, so the
    // output can keep it alive for as long as it refers to it.
    pExtractorOutput->seekMap(shared_from_this());
  }
}

std::shared_ptr<MediaFormat> FfmpegExtractor::getTrackFormat(int index) {
  if (index >= pFormatContext->nb_streams) {
    return nullptr;
  }

  std::shared_ptr<MediaFormat> format = nullptr;
  auto stream = pFormatContext->streams[index];
  auto codecpar = stream->codecpar;
  auto mime = FfmpegUtils::convertToMime(codecpar->codec_id);
  if (mime.empty()) {
    return nullptr;
  }

  if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
    format = MediaFormat::CreateAudioFormat(mime, codecpar->sample_rate, ffChannels(codecpar));
    if (codecpar->frame_size > 0) {
      auto inputSize = av_samples_get_buffer_size(
          nullptr, ffChannels(codecpar), codecpar->frame_size,
          (AVSampleFormat)codecpar->format, 1);
      format->setMaxInputSize(inputSize);
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

    if (codecpar->extradata != nullptr && codecpar->extradata_size > 0) {
      format->setCodecSpecificData(0, codecpar->extradata, codecpar->extradata_size);
    }

#ifndef NDEBUG
    MINILOG_DEBUG(
        "codecpar{\n codec={}, tag={}, duration={}, time_base={ {}, {} }, format={}, bit_rate={}, sample_rate={}, channels={}, frame_size={} \n}",
        avcodec_get_name(codecpar->codec_id),
        codecpar->codec_tag,
        stream->duration,
        stream->time_base.num,
        stream->time_base.den,
        av_get_sample_fmt_name((AVSampleFormat)codecpar->format),
        codecpar->bit_rate,
        codecpar->sample_rate,
        ffChannels(codecpar),
        codecpar->frame_size);
#endif
  }

  if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
    format = MediaFormat::CreateVideoFormat(mime, codecpar->width, codecpar->height);

    auto pixelFormat = (AVPixelFormat)codecpar->format;
    switch (pixelFormat) {
      case AV_PIX_FMT_YUV420P: {
        format->setPixelFormat(PixelFormat::kFormatI420);
        break;
      }
      case AV_PIX_FMT_NV12: {
        format->setPixelFormat(PixelFormat::kFormatNV12);
        break;
      }
      case AV_PIX_FMT_RGBA: {
        format->setPixelFormat(PixelFormat::kFormatRGBA);
        break;
      }
      default: {
        format->setPixelFormat(PixelFormat::kFormatInvalid);
        break;
      }
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

    int rotateDegree = 0;
    if (stream->metadata != nullptr) {
      auto entry =
          av_dict_get(stream->metadata, "rotate", nullptr, AV_DICT_MATCH_CASE);
      if (entry && entry->value) {
        rotateDegree = atoi(entry->value);
      }
    }
    format->setRotation(rotateDegree);

    if (codecpar->codec_id == AV_CODEC_ID_H264) {
      pVideoStreamFilter = av_bsf_get_by_name("h264_mp4toannexb");
    } else if (codecpar->codec_id == AV_CODEC_ID_HEVC) {
      pVideoStreamFilter = av_bsf_get_by_name("hevc_mp4toannexb");
    } else {
      pVideoStreamFilter = nullptr;
    }

    if (pVideoStreamFilter != nullptr) {
      AVBSFContext *videoStreamContext = nullptr;
      av_bsf_alloc(pVideoStreamFilter, &videoStreamContext);
      avcodec_parameters_copy(videoStreamContext->par_in, codecpar);
      av_bsf_init(videoStreamContext);
      pVideoStreamContext.reset(videoStreamContext);
    } else {
      pVideoStreamContext = nullptr;
    }

    if (codecpar->extradata != nullptr && codecpar->extradata_size > 0) {
      format->setCodecSpecificData(0, codecpar->extradata, codecpar->extradata_size);
    }

    int frameRate = (int)av_q2d(stream->avg_frame_rate);
    format->setFrameRate(frameRate);
#ifndef NDEBUG
    MINILOG_DEBUG(
        "codecpar {\n id={}, tag=, , time_base={ {}, {} }, format={}, bit_rate={}, frame_rate={}, width={}, height={}",
        avcodec_get_name(codecpar->codec_id),
        codecpar->codec_tag,
        stream->time_base.num,
        stream->time_base.den,
        av_get_pix_fmt_name((AVPixelFormat)codecpar->format),
        codecpar->bit_rate,
        frameRate,
        codecpar->width,
        codecpar->height);
#endif
  }

  if (format != nullptr) {
    if (codecpar->bit_rate > 0) {
      format->setAverageBitrate((int)codecpar->bit_rate);
    }
    if (stream->duration != AV_NOPTS_VALUE) {
      auto durationUs = av_rescale_q(stream->duration, stream->time_base, AVRational{1, AV_TIME_BASE});
      format->setDurationUs(durationUs);
    }
    format->setTrackId(stream->id);
  }

  return format;
}

bool FfmpegExtractor::seekTo(int64_t timeUs, SeekOption mode) {
  int ret;
  int seekFlags = AVSEEK_FLAG_BACKWARD;
  int64_t minSeekTime = INT64_MIN;
  int64_t maxSeekTime = INT64_MAX;
  int64_t seekTime;
  if (pFormatContext == nullptr) {
    return false;
  }

  seekTime = av_rescale(timeUs, AV_TIME_BASE, Definitions::kMicrosPerSecond);
  if (pFormatContext->start_time != AV_NOPTS_VALUE) {
    seekTime += pFormatContext->start_time;
  }

  switch (mode) {
    case SeekOption::SEEK_TO_PREVIOUS_SYNC: {
      maxSeekTime = seekTime;
      break;
    }
    case SeekOption::SEEK_TO_NEXT_SYNC: {
      minSeekTime = seekTime;
      break;
    }
    case SeekOption::SEEK_TO_CLOSEST_SYNC: {
      break;
    }
    default: {
      seekFlags = 0;
      break;
    }
  }

  // seek an audio track preferred.
  auto streamIndex = -1;
  if (mHasAudioTrack) {
    for (auto &track : tracks) {
      if (track.getMediaType() == MediaType::kMediaTypeAudio) {
        streamIndex = track.getTrackIndex();
        break;
      }
    }
  } else if (mHasVideoTrack) {
    for (auto &track : tracks) {
      if (track.getMediaType() == MediaType::kMediaTypeVideo) {
        streamIndex = track.getTrackIndex();
        break;
      }
    }
  }
  ret = avformat_seek_file(pFormatContext.get(), streamIndex, minSeekTime,
      seekTime, maxSeekTime, seekFlags);
  if (ret < 0) {
    MINILOG_ERROR("Failed to seek to time: {}, error: {}", timeUs, ffErrorString(ret));
    return false;
  }
  mReadingPositionUs = timeUs;
  return true;
}

void FfmpegExtractor::clear() {
  pVideoStreamFilter = nullptr;
  pVideoStreamContext.reset();

  for (auto &track : tracks) {
    track.clear();
  }
  tracks.erase(tracks.begin(), tracks.end());
  tracks.clear();
  mNeedPrepareFormat = true;
  // The format context is closed before the io context it reads through, and the buffer of that context
  // is the one this extractor allocated: a context created with AVFMT_FLAG_CUSTOM_IO owns neither.
  pFormatContext.reset();
  if (pIoContext != nullptr) {
    av_freep(&pIoContext->buffer);
    av_freep(&pIoContext);
  }
  mInput.reset();
}

bool FfmpegExtractor::isSeekable() const {
  return pFormatContext != nullptr && pFormatContext->duration != AV_NOPTS_VALUE;
}

int64_t FfmpegExtractor::getDurationUs() const {
  if (pFormatContext != nullptr && pFormatContext->duration != AV_NOPTS_VALUE) {
    return pFormatContext->duration;
  }
  return Definitions::kTimeUnset;
}

coral::SeekPoints FfmpegExtractor::getPlatformSeekPoints(int64_t timeUs) const {
  auto first = coral::SeekPoint(timeUs, INT64_MAX);
  if (pFormatContext != nullptr) {

  }
  return SeekPoints(first);
}

void FfmpegExtractor::parseVideoBitStream(AVPacket *pkt) {
  if (av_bsf_send_packet(pVideoStreamContext.get(), pkt) != 0) {
    return;
  }
  av_bsf_receive_packet(pVideoStreamContext.get(), pkt);
}

void FfmpegExtractor::notifyEndOfStream() {
  for (auto &track : tracks) {
    track.enqueueEndOfStream();
  }
}

int64_t FfmpegExtractor::getReadPosition() const {
  if (mReadingPosition != -1) {
    return mReadingPosition;
  }
  return Definitions::kPositionUnset;
}

bool FfmpegExtractor::isRealTime(AVFormatContext *pFormatCtx) {
  if (!pFormatCtx) {
    return false;
  }
  if (!strcmp(pFormatCtx->iformat->name, "rtp")
      || !strcmp(pFormatCtx->iformat->name, "rtsp")
      || !strcmp(pFormatCtx->iformat->name, "sdp")) {
    return true;
  }
  if (pFormatCtx->pb &&
      (
          !strncmp(pFormatCtx->url, "rtp:", 4)
              || !strncmp(pFormatCtx->url, "udp:", 4))) {
    return true;
  }
  return false;
}

//--------------------------------------------------------------------------------------------------

void FfmpegExtractor::FfmpegTrack::changeFormat(const std::shared_ptr<MediaFormat> &format) {
  if (trackOutput != nullptr) {
    trackOutput->format(format);
  }
}

void FfmpegExtractor::FfmpegTrack::enqueuePacketData(AVPacket *avPacket, int64_t timeUs) {
  if (trackOutput != nullptr && avPacket->size > 0 && avPacket->data != nullptr) {
    auto flags = AVBufferFlags::kNone;
    if (avPacket->flags & AV_PKT_FLAG_KEY) {
      flags = AVBufferFlags::kKeyFrame;
    } else if (avPacket->flags & AV_PKT_FLAG_DISCARD) {
      flags = AVBufferFlags::kDiscard;
    }
    // The sample owns a copy of the packet of ffmpeg, whose bytes are only valid until it is read
    // again: the output is free to keep the sample for as long as it needs it.
    trackOutput->sample(AVSample(trackId, timeUs, toSampleFlags(flags),
        std::make_shared<AVBlockBuffer>(avPacket->data, static_cast<uint32_t>(avPacket->size))));
  }
}

void FfmpegExtractor::FfmpegTrack::enqueueEndOfStream() {
  if (trackOutput != nullptr) {
    trackOutput->sample(AVSample(trackId, Definitions::kTimeEndOfSource, AVSample::kFlagEndOfStream,
        nullptr));
  }
}

FfmpegExtractorFactory::FfmpegExtractorFactory() = default;

FfmpegExtractorFactory::~FfmpegExtractorFactory() = default;

std::shared_ptr<AVMediaExtractor> FfmpegExtractorFactory::createExtractor(const std::string &url, bool platform) {
  return FfmpegExtractor::createWithUrl(url);
}

} // namespace coral
