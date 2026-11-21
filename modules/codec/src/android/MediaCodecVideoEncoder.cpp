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

#include "MediaCodecVideoEncoder.h"

#include "MediaCodecSurface.h"
#include "MediaCodecUtils.h"
#include "NativeSurface.h"

#include <media/Format.h>
#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <core/jni/android/MediaCodec.h>
#include <core/jni/android/MediaFormat.h>

#include <minilog/log.h>

#include <sys/system_properties.h>

#include <cassert>
#include <utility>

namespace coral {

MediaCodecVideoEncoder::MediaCodecVideoEncoder(const std::shared_ptr<SurfaceHolder>& surfaceHolder) noexcept
  : surfaceHolder(surfaceHolder),
    inputIndex(INDEX_UNSET),
    inputEof(false),
    outputEof(false) {
  char type[256] = { 0 };
  __system_property_get("ro.product.model", type);
  phoneType = std::string(type);

  char cpu[256] = { 0 };
  __system_property_get("ro.hardware", cpu);
  cpuType = std::string(cpu);

  MINILOG_DEBUG("phone type: {}, cpu: {}", phoneType, cpuType);
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

MediaCodecVideoEncoder::~MediaCodecVideoEncoder() {
  videoCodec.reset();
  outputFormat.reset();
}

bool MediaCodecVideoEncoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the decoder: Format is null");
    return false;
  }
  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  auto width = format->getWidth();
  auto height = format->getHeight();

  if (format->getPixelFormat() == PixelFormat::kFormatI420) {
    sampleBuffer = std::make_shared<I420PixelBuffer>(width, height);
  } else if (format->getPixelFormat() == PixelFormat::kFormatNV12) {
    sampleBuffer = std::make_shared<NV12PixelBuffer>(width, height);
  }

  auto codecFormat = JniMediaFormat::create();
  codecFormat->setString(JniMediaFormat::KEY_MIME, mimeType);
  codecFormat->setInteger(JniMediaFormat::KEY_WIDTH, (int32_t) width);
  codecFormat->setInteger(JniMediaFormat::KEY_HEIGHT, (int32_t) height);
  auto bitrate = format->getBitrate();
  if (bitrate == MediaFormat::NO_VALUE) {
    bitrate = (int32_t) (width * height * DEFAULT_BITRATE_SCALE);
  }
  codecFormat->setInteger(JniMediaFormat::KEY_BIT_RATE, bitrate);
  codecFormat->setInteger(JniMediaFormat::KEY_BITRATE_MODE, MediaCodecUtils::BITRATE_MODE_CBR);
  auto frameRate = DEFAULT_FRAME_RATE;
  if (format->getFrameRate() > 0) {
    frameRate = format->getFrameRate();
  }
  codecFormat->setInteger(JniMediaFormat::KEY_FRAME_RATE, frameRate);
  codecFormat->setInteger(JniMediaFormat::KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL);

  // If we use surface for rendering input frame, use the color format surface
  if (surfaceHolder != nullptr) {
    codecFormat->setInteger(JniMediaFormat::KEY_COLOR_FORMAT, MediaCodecUtils::COLOR_FormatSurface);
  } else {
    if (isMtkCpu()) {
      codecFormat->setInteger(JniMediaFormat::KEY_COLOR_FORMAT,
        MediaCodecUtils::COLOR_FormatYUV420Planar);
    } else {
      codecFormat->setInteger(JniMediaFormat::KEY_COLOR_FORMAT,
        MediaCodecUtils::COLOR_FormatYUV420SemiPlanar);
    }
  }

  if (MediaCodecUtils::getApiLevel() >= __ANDROID_API_Q__) {
    codecFormat->setInteger(JniMediaFormat::KEY_MAX_B_FRAMES, 0);
  }

  videoCodec = JniMediaCodec::createEncoderByType(mimeType);
  videoCodec->configure(codecFormat.get(), nullptr, nullptr, JniMediaCodec::CONFIGURE_FLAG_ENCODE);

  // checks whether we encode video frame from surface
  if (surfaceHolder != nullptr) {
    // Call from rendering with an ANativeWindow
    if (surfaceHolder->surfaceType() == SurfaceHolder::kNativeWindow) {
      surfaceHolder->surface = std::make_shared<NativeSurface>(videoCodec->createInputSurface());
    } else {
      surfaceHolder->surface = std::make_shared<MediaCodecSurface>(videoCodec->createInputSurface());
    }
  }

  inputEof = false;
  outputEof = false;

  return true;
}

void MediaCodecVideoEncoder::start() {
  if (videoCodec) {
    videoCodec->start();
  }
}

void MediaCodecVideoEncoder::stop() {
  if (videoCodec) {
    videoCodec->stop();
  }
}

std::shared_ptr<AVPixelBuffer> MediaCodecVideoEncoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !videoCodec || surfaceHolder != nullptr) {
    return nullptr;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = (int) videoCodec->dequeueInputBuffer(TIME_OUT_US);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? sampleBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void MediaCodecVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  if (!videoCodec || !inputBuffer) {
    return;
  }
  assert(inputBuffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (inputBuffer->isEndOfStream()) {
    if (__builtin_available(android __ANDROID_API_O__, *)) {
      if (surfaceHolder) {
        videoCodec->signalEndOfInputStream();
      } else {
        videoCodec->queueInputBuffer(inputIndex, 0, 0, 0, JniMediaCodec::BUFFER_FLAG_END_OF_STREAM);
      }
    } else {
      videoCodec->queueInputBuffer(inputIndex, 0, 0, 0, JniMediaCodec::BUFFER_FLAG_END_OF_STREAM);
    }
    inputIndex = INDEX_UNSET;
    inputEof = true;
  } else if (inputBuffer->isDiscard()) {
    MINILOG_DEBUG("discard input video frames: {}", inputBuffer->timeUs());
  } else if (inputBuffer->getFormat() == PixelFormat::kFormatSurface) {
    // Do nothing
  } else {
    size_t bufSize = 0;
    auto yuvBuffer = inputBuffer->asI420PixelBuffer();
    if (yuvBuffer != nullptr) {
      auto input = videoCodec->getInputBuffer(inputIndex, &bufSize);
      bufSize = std::min(yuvBuffer->size(), bufSize);
      if (isMtkCpu()) {
        memcpy(input, yuvBuffer->getData(), bufSize);
      } else {
        MediaCodecUtils::I420toYUV420SemiPlanar(
          yuvBuffer->getData(), 0, input, yuvBuffer->width(), yuvBuffer->height());
      }
    } else {
      auto nv12Buffer = inputBuffer->asNV12PixelBuffer();
      if (nv12Buffer != nullptr) {
        auto input = videoCodec->getInputBuffer(inputIndex, &bufSize);
        bufSize = std::min(nv12Buffer->size(), bufSize);
        if (isMtkCpu()) {
          MediaCodecUtils::NV12toYUV420Planar(
            nv12Buffer->getData(), 0, input, nv12Buffer->width(), nv12Buffer->height());
        } else {
          memcpy(input, nv12Buffer->getData(), bufSize);
        }
      }
    }
    videoCodec->queueInputBuffer(inputIndex, 0, (int) bufSize, inputBuffer->timeUs(), 0);
    inputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVPacketBuffer> MediaCodecVideoEncoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPacketBuffer> queuedOutputBuffer = nullptr;
  if (!videoCodec) {
    return nullptr;
  }
  packetBuffer->clear();
  if (outputEof) {
    queuedOutputBuffer = packetBuffer;
    queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    return queuedOutputBuffer;
  }

  JniMediaCodec::BufferInfo info;
  while (!outputEof) {
    auto outputIndex = videoCodec->dequeueOutputBuffer(&info, 0);
    if (outputIndex == JniMediaCodec::INFO_TRY_AGAIN_LATER) {
      break;
    } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_FORMAT_CHANGED) {
      auto format = videoCodec->getOutputFormat();
      if (format != nullptr) {
        outputFormat = MediaCodecUtils::convertToMediaFormat(format);
      }
      queuedOutputBuffer = packetBuffer;
      queuedOutputBuffer->setFormat(getOutputFormat());
      break;
    } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_BUFFERS_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_BUFFERS_CHANGED.");
    } else if (outputIndex < 0) {
      MINILOG_DEBUG("dequeue output buffer is unavailable, unknown error: {}", outputIndex);
    } else {
      queuedOutputBuffer = packetBuffer;
      if (info.flags & JniMediaCodec::BUFFER_FLAG_END_OF_STREAM) {
        outputEof = true;
        queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
      } else {
        if (info.flags & JniMediaCodec::BUFFER_FLAG_CODEC_CONFIG) {
          queuedOutputBuffer->setFlags(AVBufferFlags::kCodecConfig);
        } else if (info.flags & MediaCodecUtils::BUFFER_FLAG_KEY_FRAME) {
          queuedOutputBuffer->setFlags(AVBufferFlags::kKeyFrame);
        }
        size_t bufSize;
        auto encodeBuffer = videoCodec->getOutputBuffer(outputIndex, &bufSize);
        if (encodeBuffer) {
          if (info.flags & JniMediaCodec::BUFFER_FLAG_KEY_FRAME) {
            queuedOutputBuffer->addFlag(AVBufferFlags::kKeyFrame);
          }
          queuedOutputBuffer->timeUs(info.presentationTimeUs);
          queuedOutputBuffer->ensureSpaceForWrite(info.size);
          auto const& byteBuffer = queuedOutputBuffer->data();
          byteBuffer->position(0);
          byteBuffer->put(encodeBuffer.get() + info.offset, info.size);
          byteBuffer->flip();
        } else {
          queuedOutputBuffer->shouldBeSkipped = true;
        }
      }
      videoCodec->releaseOutputBuffer(outputIndex, false);
      break;
    }
  }

  /**
   * If we can't drain any output buffer and input is end of stream, create an output buffer with
   * end of stream.
   */
  if (!queuedOutputBuffer && inputEof) {
    outputEof = true;
    queuedOutputBuffer = packetBuffer;
    queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  }

  return queuedOutputBuffer;
}

void MediaCodecVideoEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void MediaCodecVideoEncoder::signalEndOfInputStream() {
  if (!videoCodec) {
    inputEof = true;
    return;
  }
  if (surfaceHolder) {
    videoCodec->signalEndOfInputStream();
  } else {
    auto buffer = dequeueInputBuffer();
    if (buffer != nullptr) {
      buffer->clear();
      buffer->setFlags(AVBufferFlags::kEndOfStream);
      queueInputBuffer(buffer);
    }
  }
  inputIndex = INDEX_UNSET;
  inputEof = true;
}

void MediaCodecVideoEncoder::flush() {
  if (videoCodec) {
    videoCodec->flush();
  }
  inputEof = false;
  outputEof = false;
  inputIndex = INDEX_UNSET;
}

void MediaCodecVideoEncoder::release() {
  if (videoCodec) {
    videoCodec->stop();
    videoCodec->release();
  }
  videoCodec.reset();
  inputEof = true;
  outputEof = true;
  inputIndex = INDEX_UNSET;
}

} // namespace coral
