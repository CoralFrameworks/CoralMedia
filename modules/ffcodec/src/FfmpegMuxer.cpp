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

#include <ffcodec/FfmpegMuxer.h>

#include <media/Types.h>

#include <minilog/log.h>

namespace coral {

FfmpegMuxer::FfmpegMuxer() : pFormatContext(nullptr) {
  av_register_all();
  mCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

FfmpegMuxer::~FfmpegMuxer() {
  closeMuxer();
  mCachePacket.reset();
}

bool FfmpegMuxer::prepare(std::string url, const MuxerFormat format) {
  if (format == MuxerFormat::kMuxerFormatInvalid) {
    MINILOG_ERROR("Failed to prepare: the muxer format is invalid");
    return false;
  }

  if (isStarted()) {
    MINILOG_ERROR("Failed to prepare: the muxer is started");
    return false;
  }

  mOutputUrl = std::move(url);
  if (mOutputUrl.empty()) {
    MINILOG_ERROR("Failed to prepare muxer: output url is empty");
    return false;
  }
  auto formatStr = getMuxerFormatName(format);
  AVFormatContext *formatContext = nullptr;
  auto ret = avformat_alloc_output_context2(&formatContext, nullptr, formatStr,
      mOutputUrl.c_str());
  if (!formatContext || ret < 0) {
    MINILOG_ERROR("Failed to create muxer context: {}", av_err2str(ret));
    return false;
  }
  pFormatContext.reset(formatContext);
  mStatus = Status::kMuxerStatusPrepared;
  return true;
}

int FfmpegMuxer::addTrack(const std::shared_ptr<MediaFormat> &format) {
  if (isStarted()) {
    MINILOG_ERROR("Failed to add track: the muxer is started");
    return kInvalidTrack;
  }

  int bitrate = format->getBitrate() == Definitions::kInvalidValue
      ? DEFAULT_AUDIO_BIT_RATE
      : format->getBitrate();

  auto const &codecID = FfmpegUtils::convertToCodecID(format->getMimeType());
  if (format->isAudio()) {
    return addAudioTrack(codecID, format->getSampleRate(), format->getChannelCount(), bitrate);
  } else if (format->isVideo()) {
    int frameRate = format->getFrameRate() == Definitions::kInvalidValue
        ? DEFAULT_FRAME_RATE
        : format->getFrameRate();
    return addVideoTrack(codecID, format->getWidth(), format->getHeight(), frameRate, bitrate);
  }

  return kInvalidTrack;
}

void FfmpegMuxer::writeFrame(const std::shared_ptr<AVPacketBuffer> &buffer) {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to write frame: muxer context is null");
    return;
  }

  if (!isStarted()) {
    MINILOG_ERROR("Failed to write frame: muxer is not started");
    return;
  }

  if (!buffer) {
    MINILOG_ERROR("no media data to write.");
    return;
  }

  // write extra data to muxer
  if (buffer->isCodecConfig()) {
    writeExtraData(buffer->streamIndex(), buffer->array(), buffer->size());
    return;
  } else if (buffer->isEndOfStream()) {
    av_interleaved_write_frame(pFormatContext.get(), nullptr);
    return;
  }

  // write packet to muxer
  av_packet_unref(mCachePacket.get());
  if (mCachePacket->data != nullptr) {
    mCachePacket->data = (uint8_t *)av_realloc(mCachePacket->data, buffer->size());
  } else {
    mCachePacket->data = (uint8_t *)av_malloc(buffer->size());
  }
  mCachePacket->size = (int)buffer->size();
  mCachePacket->stream_index = buffer->streamIndex();
  auto time_base = pFormatContext->streams[mCachePacket->stream_index]->time_base;
  mCachePacket->pts = av_rescale_q(buffer->timeUs(), AV_TIME_BASE_Q, time_base);
  memcpy(mCachePacket->data, buffer->array(), buffer->size());
  auto ret = av_interleaved_write_frame(pFormatContext.get(), mCachePacket.get());
  if (ret < 0) {
    MINILOG_ERROR(
        "Failed to write buffer to the muxer: {}, pts: {}, buffer timeUs: {}, buffer size: {}, media type: {}",
        av_err2str(ret), mCachePacket->pts, buffer->timeUs(), buffer->size(), convertToText(buffer->getMediaType()));
  }
}

void FfmpegMuxer::start() {
  if (isStarted()) {
    return;
  }
  int ret = openMuxer();
  if (ret < 0) {
    MINILOG_ERROR("Failed to open ffmpeg muxer");
    return;
  }
  ret = writeHeader();
  if (ret < 0) {
    MINILOG_ERROR("Failed to write header");
    mStatus = Status::kMuxerStatusError;
  } else {
    mStatus = Status::kMuxerStatusStarted;
  }
}

void FfmpegMuxer::stop() {
  if (!isStarted()) {
    return;
  }
  int ret = writeTrailer();
  if (ret < 0) {
    MINILOG_ERROR("Failed to write trailer");
  }
  mStatus = Status::kMuxerStatusStopped;
}

void FfmpegMuxer::release() {
  closeMuxer();
}

const char *FfmpegMuxer::getName() const {
  return "FfmpegMuxer";
}

int FfmpegMuxer::openMuxer() {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to open muxer: context is null");
    return -1;
  }

  int ret;
  if (!(pFormatContext->oformat->flags & AVFMT_NOFILE)) {
    if ((ret = avio_open(&pFormatContext->pb, mOutputUrl.c_str(), AVIO_FLAG_WRITE)) < 0) {
      MINILOG_ERROR("Failed to open output file: {}", mOutputUrl);
      return ret;
    }
  }
  return 0;
}

int FfmpegMuxer::addAudioTrack(AVCodecID codecId, int sampleRate,
    int channelCount, int bitRate) {
  if (codecId == AV_CODEC_ID_NONE) {
    return kInvalidTrack;
  }
  auto stream = createStream(codecId);
  if (stream != nullptr) {
    stream->time_base = (AVRational){1, sampleRate};
    stream->codecpar->sample_rate = sampleRate;
    stream->codecpar->channels = channelCount;
    stream->codecpar->channel_layout =
        (uint64_t)av_get_default_channel_layout(channelCount);
    stream->codecpar->bit_rate = bitRate;
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = codecId;
    stream->codecpar->codec_tag = 0;
    return stream->index;
  }
  return kInvalidTrack;
}

int FfmpegMuxer::addVideoTrack(AVCodecID codecId, int width, int height,
    int frameRate, int bitRate) {
  if (codecId == AV_CODEC_ID_NONE) {
    return kInvalidTrack;
  }
  auto stream = createStream(codecId);
  if (stream != nullptr) {
    stream->time_base = av_d2q(1.0 / frameRate, 120);
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    stream->codecpar->bit_rate = bitRate;
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = codecId;
    stream->codecpar->codec_tag = 0;
    return stream->index;
  }
  return kInvalidTrack;
}

AVStream *FfmpegMuxer::createStream(AVCodecID id) {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to create stream: muxer context is null");
    return nullptr;
  }

  if (id == AV_CODEC_ID_NONE) {
    MINILOG_ERROR("Failed to find encoder: {}", avcodec_get_name(id));
    return nullptr;
  }

  // find encoder for the muxer
  auto encoder = avcodec_find_encoder(id);
  if (encoder == nullptr) {
    MINILOG_ERROR("Failed to find encoder: ", avcodec_get_name(id));
    return nullptr;
  }
  return avformat_new_stream(pFormatContext.get(), encoder);
}

int FfmpegMuxer::writeHeader() {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to writer header: muxer context is null");
    return -1;
  }
  if (!(pFormatContext->oformat->flags & AVFMT_GLOBALHEADER)) {
    return 0;
  }
  int ret = avformat_write_header(pFormatContext.get(), nullptr);
  if (ret < 0) {
    MINILOG_ERROR("Failed to call avformat_write_header: ");
    return ret;
  }
  return 0;
}

bool FfmpegMuxer::writeExtraData(int trackIndex, uint8_t *extraData,
    size_t size) {
  if (trackIndex < 0 || trackIndex >= pFormatContext->nb_streams ||
      !extraData || size <= 0) {
    MINILOG_ERROR("Failed to write extra data");
    return false;
  }

  auto stream = pFormatContext->streams[trackIndex];
  if (stream != nullptr && stream->codecpar != nullptr) {
    stream->codecpar->extradata =
        (uint8_t *)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(stream->codecpar->extradata, extraData, size);
    stream->codecpar->extradata_size = static_cast<int>(size);
    return true;
  }

  MINILOG_ERROR("Failed to write extra data: stream is empty");
  return false;
}

int FfmpegMuxer::writeTrailer() {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to find muxer context");
    return -1;
  }
  int ret = av_write_trailer(pFormatContext.get());
  if (ret < 0) {
    MINILOG_ERROR("Failed to call av_write_trailer: {}", av_err2str(ret));
    return ret;
  }
  return 0;
}

void FfmpegMuxer::closeMuxer() {
  if (pFormatContext) {
    MINILOG_DEBUG("Close file success: {}", mOutputUrl);
  }
  pFormatContext.reset();
  mStatus = Status::kMuxerStatusIdle;
}

void FfmpegMuxer::setMetadata(const char *key, const char *val) {
  if (pFormatContext) {
    av_dict_set(&pFormatContext->metadata, key, val, 0);
  }
}

const char *FfmpegMuxer::getMuxerFormatName(MuxerFormat format) {
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4: {
      return "mp4";
    }
    case MuxerFormat::kMuxerFormatWebm: {
      return "webm";
    }
    case MuxerFormat::kMuxerFormat3Gpp: {
      return "3gp";
    }
    case MuxerFormat::kMuxerFormatOgg: {
      return "ogg";
    }
    case MuxerFormat::kMuxerFormatOpus: {
      return "opus";
    }
    case MuxerFormat::kMuxerFormatFlac: {
      return "flac";
    }
    case MuxerFormat::kMuxerFormatH263: {
      return "h263";
    }
    case MuxerFormat::kMuxerFormatH264: {
      return "h264";
    }
    default: {
      return "";
    }
  }
}

FfmpegMuxerFactory::FfmpegMuxerFactory() = default;

FfmpegMuxerFactory::~FfmpegMuxerFactory() = default;

bool FfmpegMuxerFactory::isSupportMuxerFormat(const MuxerFormat muxerFormat) {
  return muxerFormat != MuxerFormat::kMuxerFormatInvalid;
}

std::unique_ptr<AVMediaMuxer> FfmpegMuxerFactory::createMuxer() {
  return std::make_unique<FfmpegMuxer>();
}

} // namespace coral
