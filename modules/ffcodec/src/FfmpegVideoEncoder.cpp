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

#include <ffcodec/FfmpegVideoEncoder.h>

#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <libyuv.h>

#include <utility>

namespace coral {

FfmpegVideoEncoder::FfmpegVideoEncoder()
    : running(false),
      requestEof(false),
      flushed(false),
      pCodecContext(nullptr) {
  cacheFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
  cachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

FfmpegVideoEncoder::~FfmpegVideoEncoder() {
  cacheFrame.reset();
  cachePacket.reset();
  mediaFormat.reset();
}

bool FfmpegVideoEncoder::configure(const std::shared_ptr<MediaFormat> &format,
  const std::shared_ptr<VideoSurface> &surface) {
  mediaFormat = format;
  auto result = initEncoder();
  if (result) {
    running = false;
    flushed = false;
  }
  return result;
}

void FfmpegVideoEncoder::start() {
  if (!pCodecContext) {
    MINILOG_ERROR("Failed to configure before start encoding");
    return;
  }
  if (thread == nullptr) {
    thread = std::make_unique<std::thread>(&FfmpegVideoEncoder::run, this);
  }
}

void FfmpegVideoEncoder::stop() {
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (running) {
      running = false;
      lock.unlock();
    }
    notify_cv_.notify_all();
  }
  if (thread) {
    if (thread->joinable()) {
      thread->join();
    } else {
      thread->detach();
    }
  }
  thread.reset();
}

void FfmpegVideoEncoder::flush() {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (running) {
    flushed = true;
    lock.unlock();
    notify_cv_.notify_all();
  } else {
    flushed = false;
    flushInternal();
  }
}

void FfmpegVideoEncoder::release() {
  stop();
  pCodecContext.reset();
  clearBufferQueuesLocked();
}

std::shared_ptr<AVPixelBuffer> FfmpegVideoEncoder::dequeueInputBuffer() {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (availableInputBuffers.empty()) {
    auto pixelFormat = mediaFormat->getPixelFormat();
    if (pixelFormat == PixelFormat::kFormatI420) {
      return std::make_shared<I420PixelBuffer>(mediaFormat->getWidth(), mediaFormat->getHeight());
    } else if (pixelFormat == PixelFormat::kFormatNV12) {
      return std::make_shared<NV12PixelBuffer>(mediaFormat->getWidth(), mediaFormat->getHeight());
    }
    return nullptr;
  } else {
    auto buffer = std::move(availableInputBuffers.front());
    availableInputBuffers.pop_front();
    return buffer;
  }
}

void FfmpegVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> buffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  maybeNotifyError();
  if (buffer != nullptr) {
    queuedInputBuffers.push_back(buffer);
    maybeNotifyProcessLoop();
  }
}

std::shared_ptr<AVPacketBuffer> FfmpegVideoEncoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPacketBuffer> outputBuffer = nullptr;
  std::unique_lock<std::mutex> lock(pending_lock_);
  maybeNotifyError();
  if (!queuedOutputBuffers.empty()) {
    outputBuffer = std::move(queuedOutputBuffers.front());
    queuedOutputBuffers.pop_front();
  }
  return outputBuffer;
}

void FfmpegVideoEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void FfmpegVideoEncoder::signalEndOfInputStream() {
  auto inputBuffer = dequeueInputBuffer();
  if (inputBuffer != nullptr) {
    inputBuffer->clear();
    inputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(inputBuffer);
  } else {
    std::unique_lock<std::mutex> lock(pending_lock_);
    requestEof = true;
    maybeNotifyProcessLoop();
  }
}

void FfmpegVideoEncoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (inputBuffer != nullptr) {
    inputBuffer->clear();
    availableInputBuffers.push_back(std::move(inputBuffer));
    maybeNotifyProcessLoop();
  }
}

void FfmpegVideoEncoder::recycleBuffer(std::shared_ptr<AVPacketBuffer> packetBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (packetBuffer != nullptr) {
    packetBuffer->clear();
    availableOutputBuffers[availableOutputBufferCount++] = std::move(packetBuffer);
    maybeNotifyProcessLoop();
  }
}

bool FfmpegVideoEncoder::encode() {
  std::shared_ptr<AVPixelBuffer> inputBuffer;
  bool flushEncoder;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    while (running && !canProcessBuffer()) {
      notify_cv_.wait(lock);
    }
    if (!running) {
      return false;
    }
    inputBuffer = queuedInputBuffers.front();
    queuedInputBuffers.pop_front();
    flushEncoder = flushed;
    flushed = false;
  }

  if (flushEncoder) {
    flushInternal();
  }

  // discard frame
  if (inputBuffer->isDiscard()) {
    inputBuffer->release();
    return true;
  }

  // queue end of stream frame
  if (inputBuffer->isEndOfStream()) {
    inputBuffer->release();
    auto outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    queueOutputBuffer(std::move(outputBuffer));
    return true;
  }

  // prepare cache fame
  cacheFrame->pts = av_rescale_q(inputBuffer->timeUs(), AVRational{1, AV_TIME_BASE}, pCodecContext->time_base);
  // encode video frame
  if (inputBuffer->getFormat() == PixelFormat::kFormatI420) {
    auto i420Buffer = (I420PixelBuffer *)(inputBuffer.get());
    auto ret = libyuv::I420Copy(
        i420Buffer->dataY(), i420Buffer->strideY(), i420Buffer->dataU(),
        i420Buffer->strideU(), i420Buffer->dataV(), i420Buffer->strideV(),
        cacheFrame->data[0], cacheFrame->linesize[0], cacheFrame->data[1],
        cacheFrame->linesize[1], cacheFrame->data[2], cacheFrame->linesize[2],
        cacheFrame->width, cacheFrame->height);
    if (ret == 0) {
      encodeFrame(cacheFrame.get());
    }
  } else if (inputBuffer->getFormat() == PixelFormat::kFormatNV12) {
    auto nv12Buffer = (NV12PixelBuffer *)(inputBuffer.get());
    auto ret = libyuv::NV12ToI420(
        nv12Buffer->dataY(), nv12Buffer->strideY(), nv12Buffer->dataUV(),
        nv12Buffer->strideUV(), cacheFrame->data[0], cacheFrame->linesize[0],
        cacheFrame->data[1], cacheFrame->linesize[1], cacheFrame->data[2],
        cacheFrame->linesize[2], cacheFrame->width, cacheFrame->height);
    if (ret == 0) {
      encodeFrame(cacheFrame.get());
    }
  } else {
    inputBuffer->release();
    MINILOG_ERROR("Unsupported frame type to encode");
    return false;
  }
  inputBuffer->release();
  return true;
}

bool FfmpegVideoEncoder::initEncoder() {
  int ret;
  auto mimeType = mediaFormat->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("Failed to find mime type to configure video encoder");
    return false;
  }

  auto codec_id = FfmpegUtils::convertToCodecID(mimeType);
  if (codec_id == AV_CODEC_ID_NONE) {
    MINILOG_ERROR("Failed to init: codec id is {}", avcodec_get_name(codec_id));
    return false;
  }

  auto pixelFormat = FfmpegUtils::convertToAVPixelFormat(mediaFormat->getPixelFormat());
  if (pixelFormat == AV_PIX_FMT_NONE) {
    MINILOG_ERROR("Failed to find pixel format: {}", av_get_pix_fmt_name(pixelFormat));
    return false;
  }

  auto codec = avcodec_find_encoder(codec_id);
  if (!codec) {
    MINILOG_ERROR("Failed to find encoder: {}", avcodec_get_name(codec_id));
    return false;
  }

  // create codec context
  pCodecContext.reset(avcodec_alloc_context3(codec));
  if (!pCodecContext) {
    MINILOG_ERROR("Failed to allocate encoder context.");
    return false;
  }

  pCodecContext->width = mediaFormat->getWidth();
  pCodecContext->height = mediaFormat->getHeight();
  pCodecContext->pix_fmt = pixelFormat;
  auto frameRate = DEFAULT_FRAME_RATE;
  if (mediaFormat->getFrameRate() > 0) {
    frameRate = mediaFormat->getFrameRate();
  }
  pCodecContext->framerate = AVRational{frameRate, 1};
  pCodecContext->gop_size = frameRate;
  pCodecContext->time_base = av_d2q(1.0 / frameRate, 120);
  pCodecContext->max_b_frames = 0;
  pCodecContext->err_recognition = AV_EF_IGNORE_ERR;

  // open video encoder
  if ((ret = avcodec_open2(pCodecContext.get(), codec, nullptr)) < 0) {
    MINILOG_ERROR("Failed to open video encoder, result: {}", ffErrorString(ret));
    pCodecContext.reset();
    return false;
  }

  cacheFrame->width = pCodecContext->width;
  cacheFrame->height = pCodecContext->height;
  cacheFrame->format = pCodecContext->pix_fmt;
  av_image_alloc(cacheFrame->data, cacheFrame->linesize, cacheFrame->width,
      cacheFrame->height, pCodecContext->pix_fmt, 1);

  return true;
}

void FfmpegVideoEncoder::prepareFormat() {
  auto frameRate = (int)av_q2d(pCodecContext->framerate);
  if (frameRate < 0) {
    frameRate = DEFAULT_FRAME_RATE;
  }
  mediaFormat->setFrameRate(frameRate);
  if (pCodecContext->bit_rate > 0) {
    mediaFormat->setAverageBitrate(pCodecContext->bit_rate);
  }

  auto outputBuffer = dequeueAvailableOutputBuffer();
  outputBuffer->clear();
  outputBuffer->setFormat(getOutputFormat());
  queueOutputBuffer(std::move(outputBuffer));
}

void FfmpegVideoEncoder::prepareCodecConfig() {
  // copy extra data to output buffer
  if (pCodecContext != nullptr
      && pCodecContext->extradata != nullptr
      && pCodecContext->extradata_size > 0) {
    auto outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kCodecConfig);
    outputBuffer->timeUs(0);
    auto outputData = outputBuffer->data();
    outputData->position(0);
    outputData->put(pCodecContext->extradata, pCodecContext->extradata_size);
    outputData->flip();
    queueOutputBuffer(std::move(outputBuffer));
  }
}

void FfmpegVideoEncoder::flushInternal() {
  clearBufferQueuesLocked();
  if (pCodecContext) {
    avcodec_flush_buffers(pCodecContext.get());
  }
}

void FfmpegVideoEncoder::clearBufferQueuesLocked() {
  while (!queuedInputBuffers.empty()) {
    queuedInputBuffers.front()->release();
    queuedInputBuffers.pop_front();
  }
  while (!queuedOutputBuffers.empty()) {
    queuedOutputBuffers.front()->release();
    queuedOutputBuffers.pop_front();
  }
}

void FfmpegVideoEncoder::encodeFrame(AVFrame *frame) {
  int ret;

  // send video frame to encode
  ret = avcodec_send_frame(pCodecContext.get(), frame);
  if (ret < 0) {
    MINILOG_ERROR("Failed to call avcodec_send_frame: {}", ffErrorString(ret));
    return;
  }

  auto packet = cachePacket.get();
  av_packet_unref(packet);
  while (true) {
    // Receive one encoded frame from the encoder.
    ret = avcodec_receive_packet(pCodecContext.get(), packet);
    if (ret == AVERROR(EAGAIN)) {
      break;
    } else if (ret == AVERROR_EOF) {
      auto outputBuffer = dequeueAvailableOutputBuffer();
      outputBuffer->clear();
      outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
      queueOutputBuffer(std::move(outputBuffer));
      break;
    } else if (ret < 0) {
      MINILOG_ERROR("Failed to call avcodec_receive_packet: {}", ffErrorString(ret));
      break;
    } else {
      auto buffer = dequeueAvailableOutputBuffer();
      buffer->clear();
      if (packet->flags & AV_PKT_FLAG_KEY) {
        buffer->setFlags(AVBufferFlags::kKeyFrame);
      }
      auto timeUs = av_rescale_q(packet->pts, pCodecContext->time_base, AVRational{1, AV_TIME_BASE});
      buffer->timeUs(timeUs);
      auto outputBuffer = buffer->data();
      outputBuffer->position(0);
      outputBuffer->put(packet->data, packet->size);
      outputBuffer->flip();
      queueOutputBuffer(std::move(buffer));
    }
  }
}

void FfmpegVideoEncoder::releaseCodecContext(AVCodecContext *codecContext) {
  if (codecContext) {
    avcodec_free_context(&codecContext);
  }
}

} // namespace coral
