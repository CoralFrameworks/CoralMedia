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

#include "private/ffcodec/SimpleVideoDecoder.h"

#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <libyuv.h>

namespace coral {

SimpleVideoDecoder::SimpleVideoDecoder() : SimpleDecoder(NUM_BUFFERS, NUM_BUFFERS) {
  mCacheFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
  mCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

void SimpleVideoDecoder::configure(const std::shared_ptr<MediaFormat> &format) {
  mInputFormat = format;
  initDecoder(format);
  SimpleDecoder::configure(format);
  mOutputFormat = format->buildUpon();
}

void SimpleVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> decodedBuffer) {
  SimpleDecoder::releaseOutputBuffer(decodedBuffer);
}

std::shared_ptr<AVPacketBuffer> SimpleVideoDecoder::createInputBuffer() {
  return std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

std::shared_ptr<AVPixelBuffer> SimpleVideoDecoder::createOutputBuffer() {
  if (mInputFormat->getPixelFormat() == PixelFormat::kFormatNV12) {
    return std::make_shared<NV12PixelBuffer>(this, mInputFormat->getWidth(), mInputFormat->getHeight());
  } else {
    return std::make_shared<I420PixelBuffer>(this, mInputFormat->getWidth(), mInputFormat->getHeight());
  }
}

std::optional<Error>
SimpleVideoDecoder::decodeBuffer(const std::shared_ptr<AVPacketBuffer> &inputBuffer,
    const std::shared_ptr<AVPixelBuffer> &outputBuffer,
    bool reset) {
  if (reset) {
    resetDecoder();
    if (!mCodecContext) {
      return Error("Failed to reset decoder");
    }
  }

  // The packet must own its data: the decoder keeps a reference to it after avcodec_send_packet
  // returns, and the renderer reuses the input buffer for the next frame.
  auto packet = mCachePacket.get();
  av_packet_unref(packet);
  const int size = static_cast<int>(inputBuffer->size());
  if (av_new_packet(packet, size) < 0) {
    return Error("Failed to allocate the decoder packet");
  }
  std::memcpy(packet->data, inputBuffer->array(), static_cast<size_t>(size));

  // convert pts to decoder context's time base
  auto pts = inputBuffer->timeUs();
  packet->dts = packet->pts = av_rescale_q(pts, AVRational{1, AV_TIME_BASE}, mCodecContext->time_base);
  packet->stream_index = inputBuffer->streamIndex();
  if (inputBuffer->isKeyFrame()) {
    packet->flags |= AV_PKT_FLAG_KEY;
  }

  int result = decodePacket(packet, outputBuffer);
  av_packet_unref(packet);
  if (result == CODEC_ERROR_OTHER) {
    return std::nullopt;
  } else if (result == CODEC_ERROR_INVALID_DATA) {
    outputBuffer->setFlags(AVBufferFlags::kDiscard);
  } else if (result == CODEC_ERROR_EOF) {
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  }
  return std::nullopt;
}

void SimpleVideoDecoder::onRelease() {
  mCodecContext.reset();
}

void SimpleVideoDecoder::initDecoder(const std::shared_ptr<MediaFormat> &format) {
  int ret;
  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("Failed to find video mime to configure.");
    return;
  }

  auto codec_id = FfmpegUtils::convertToCodecID(mimeType);
  if (codec_id == AV_CODEC_ID_NONE) {
    MINILOG_ERROR("Failed to init: codec id is none.");
    return;
  }

  const AVCodec* codec = nullptr;
  // FFmpeg 9's native AV1 decoder only drives hardware acceleration; the software path is libdav1d.
  if (codec_id == AV_CODEC_ID_AV1) {
    codec = avcodec_find_decoder_by_name("libdav1d");
  }
  if (codec == nullptr) {
    codec = avcodec_find_decoder(codec_id);
  }
  if (!codec) {
    MINILOG_ERROR("Failed to find decoder: {}", avcodec_get_name(codec_id));
    return;
  }

  // create codec context
  auto codecContext = avcodec_alloc_context3(nullptr);
  if (!codecContext) {
    MINILOG_ERROR("Failed to allocate decoder context.");
    return;
  }

  codecContext->codec_id = codec_id;
  codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
  codecContext->width = format->getWidth();
  codecContext->height = format->getHeight();
  if (format->getFrameRate() > 0) {
    codecContext->framerate = {(int)format->getFrameRate(), 1};
  } else {
    codecContext->framerate = {DEFAULT_FRAME_RATE, 1};
  }
  codecContext->thread_count = 1;
  codecContext->thread_type = FF_THREAD_SLICE;

  // Decoder timestamps are in the packet time base; the renderer asks for microseconds, so both
  // the input rescaler and the output one need the same definition.
  codecContext->time_base = AVRational{1, 1'000'000};
  switch (format->getPixelFormat()) {
    case PixelFormat::kFormatI420: {
      codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
      break;
    }
    case PixelFormat::kFormatNV12: {
      codecContext->pix_fmt = AV_PIX_FMT_NV12;
      break;
    }
    case PixelFormat::kFormatRGBA: {
      codecContext->pix_fmt = AV_PIX_FMT_RGBA;
      break;
    }
    default: {
      codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
      break;
    }
  }

  // handle extra data
  auto const &csdBuffers = format->csdList();
  if (!csdBuffers.empty()) {
    // The CSD list holds one entry per parameter set (SPS and PPS for H.264). The decoder needs all
    // of them in extradata; using only the first one made FLV fail with "Invalid data" because the
    // PPS of its sequence header was dropped.
    size_t size = 0;
    for (const auto& buffer : csdBuffers) {
      size += buffer.size();
    }
    codecContext->extradata_size = static_cast<int>(size);
    codecContext->extradata = (uint8_t *)av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!codecContext->extradata) {
      releaseCodecContext(codecContext);
      codecContext = nullptr;
      return;
    }
    size_t offset = 0;
    for (const auto& buffer : csdBuffers) {
      memcpy(codecContext->extradata + offset, buffer.data(), buffer.size());
      offset += buffer.size();
    }
  }

  // open video decoder
  if ((ret = avcodec_open2(codecContext, codec, nullptr)) < 0) {
    MINILOG_ERROR("Failed to open video decoder, result: {}", ffErrorString(ret));
    releaseCodecContext(codecContext);
    codecContext = nullptr;
    return;
  }

  mCodecContext.reset(codecContext);
}

void SimpleVideoDecoder::resetDecoder() {
  mCodecContext.reset();
  initDecoder(mInputFormat);
}

int SimpleVideoDecoder::decodePacket(AVPacket *inputPacket, const std::shared_ptr<AVPixelBuffer> &outputBuffer) {
  int result;

  // Sends video packet to decode
  result = avcodec_send_packet(mCodecContext.get(), inputPacket);
  if (result == AVERROR(EAGAIN)) {
    // The decoder still owns a packet whose frame was not read yet. Keep it for the next call; the
    // caller treats this as "no output yet", not as a broken frame.
    return CODEC_ERROR_OTHER;
  }
  if (result < 0) {
    MINILOG_ERROR("Failed to call avcodec_send_packet: {}", ffErrorString(result));
    return result == AVERROR_INVALIDDATA ? CODEC_ERROR_INVALID_DATA : CODEC_ERROR_OTHER;
  }

  auto frame = mCacheFrame.get();
  av_frame_unref(frame);
  result = avcodec_receive_frame(mCodecContext.get(), frame);
  if (result == AVERROR(EAGAIN)) {
    // VP9/AV1 buffer packets until a frame can be reconstructed. This is not an invalid frame.
    return CODEC_ERROR_OTHER;
  }
  if (result == AVERROR_EOF) {
    return CODEC_ERROR_EOF;
  }
  if (result < 0) {
    av_frame_unref(mCacheFrame.get());
    MINILOG_ERROR("Failed to call avcodec_receive_frame: {}", ffErrorString(result));
    return CODEC_ERROR_INVALID_DATA;
  }

  // Copy to output buffer
  auto timeUs = av_rescale_q(mCacheFrame->pts, mCodecContext->time_base, AVRational{1, AV_TIME_BASE});
  outputBuffer->timeUs(timeUs);
  auto frameType = outputBuffer->getFormat();
  if (mCacheFrame->format == AV_PIX_FMT_NV12) {
    if (frameType == PixelFormat::kFormatNV12) {
      auto buffer = (NV12PixelBuffer *)(outputBuffer.get());
      if (mCacheFrame->linesize[0] == buffer->strideY()) {
        std::memcpy(buffer->mutableDataY(), mCacheFrame->data[0],
            mCacheFrame->height * buffer->strideY());
      } else {
        uint8_t *dst = buffer->mutableDataY();
        uint8_t *src = mCacheFrame->data[0];
        for (int j = 0; j < mCacheFrame->height; j++) {
          memcpy(dst, src, mCacheFrame->width);
          src += mCacheFrame->linesize[0];
          dst += buffer->strideY();
        }
      }
    } else {
      auto buffer = (I420PixelBuffer *)(outputBuffer.get());
      libyuv::NV12ToI420(mCacheFrame->data[0], mCacheFrame->linesize[0],
          mCacheFrame->data[1], mCacheFrame->linesize[1],
          buffer->mutableDataY(), buffer->strideY(),
          buffer->mutableDataU(), buffer->strideU(),
          buffer->mutableDataV(), buffer->strideV(),
          buffer->width(), buffer->height());
    }
  } else if (mCacheFrame->format == AV_PIX_FMT_YUV420P) {
    if (frameType == PixelFormat::kFormatNV12) {
      auto buffer = (NV12PixelBuffer *)(outputBuffer.get());
      libyuv::I420ToNV12(mCacheFrame->data[0], mCacheFrame->linesize[0],
          mCacheFrame->data[1], mCacheFrame->linesize[1],
          mCacheFrame->data[2], mCacheFrame->linesize[2],
          buffer->mutableDataY(), buffer->strideY(),
          buffer->mutableDataUV(), buffer->strideUV(),
          buffer->width(), buffer->height());
    } else {
      auto buffer = (I420PixelBuffer *)(outputBuffer.get());
      libyuv::I420Copy(mCacheFrame->data[0], mCacheFrame->linesize[0],
          mCacheFrame->data[1], mCacheFrame->linesize[1],
          mCacheFrame->data[2], mCacheFrame->linesize[2],
          buffer->mutableDataY(), buffer->strideY(),
          buffer->mutableDataU(), buffer->strideU(),
          buffer->mutableDataV(), buffer->strideV(),
          buffer->width(), buffer->height());
    }
  } else {
    auto cacheFrame = av_frame_alloc();
    cacheFrame->width = outputBuffer->width();
    cacheFrame->height = outputBuffer->height();
    cacheFrame->format = AV_PIX_FMT_YUV420P;
    av_image_alloc(cacheFrame->data, cacheFrame->linesize, cacheFrame->width,
        cacheFrame->height, AV_PIX_FMT_YUV420P, 1);
    if (pSwsContext == nullptr) {
      auto swsContext =
          sws_getContext(mCacheFrame->width, mCacheFrame->height,
              (AVPixelFormat)mCacheFrame->format, cacheFrame->width,
              cacheFrame->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC,
              nullptr, nullptr, nullptr);
      pSwsContext.reset(swsContext);
    }
    sws_scale(pSwsContext.get(), mCacheFrame->data, mCacheFrame->linesize, 0,
        cacheFrame->height, cacheFrame->data, cacheFrame->linesize);
    if (frameType == PixelFormat::kFormatNV12) {
      auto buffer = (NV12PixelBuffer *)(outputBuffer.get());
      libyuv::I420ToNV12(cacheFrame->data[0], cacheFrame->linesize[0],
          cacheFrame->data[1], cacheFrame->linesize[1],
          cacheFrame->data[2], cacheFrame->linesize[2],
          buffer->mutableDataY(), buffer->strideY(),
          buffer->mutableDataUV(), buffer->strideUV(),
          buffer->width(), buffer->height());
    } else {
      auto buffer = (I420PixelBuffer *)(outputBuffer.get());
      libyuv::I420Copy(cacheFrame->data[0], cacheFrame->linesize[0],
          cacheFrame->data[1], cacheFrame->linesize[1],
          cacheFrame->data[2], cacheFrame->linesize[2],
          buffer->mutableDataY(), buffer->strideY(),
          buffer->mutableDataU(), buffer->strideU(),
          buffer->mutableDataV(), buffer->strideV(),
          buffer->width(), buffer->height());
    }
    av_frame_free(&cacheFrame);
  }
  av_frame_unref(mCacheFrame.get());
  return 0;
}

void SimpleVideoDecoder::releaseCodecContext(AVCodecContext *context) {
  if (!context) {
    return;
  }

  SwsContext *swsContext;
  if ((swsContext = (SwsContext *)context->opaque)) {
    sws_freeContext(swsContext);
    context->opaque = nullptr;
  }
  avcodec_free_context(&context);
}

} // namespace coral
