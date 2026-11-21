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

#include "NativeVideoEncoder.h"

#include "MediaCodecUtils.h"
#include "NativeSurface.h"

#include <media/Format.h>
#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <sys/system_properties.h>

#include <utility>

namespace coral {

NativeVideoEncoder::NativeVideoEncoder(const std::shared_ptr<SurfaceHolder>& surfaceHolder) noexcept
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

  MINILOG_DEBUG("phone type: ｛｝, cpu: {}", phoneType, cpuType);

  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

NativeVideoEncoder::~NativeVideoEncoder() {
  videoCodec.reset();
  outputFormat.reset();
}

bool NativeVideoEncoder::configure(const std::shared_ptr<MediaFormat>& format,
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

  auto mediaFormat = AMediaFormat_new();
  AMediaFormat_setString(mediaFormat, AMEDIAFORMAT_KEY_MIME, mimeType.c_str());
  AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, (int32_t) width);
  AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, (int32_t) height);

  auto frameRate = format->getFrameRate();
  if (frameRate == MediaFormat::NO_VALUE) {
    frameRate = DEFAULT_FRAME_RATE;
  }
  AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_FRAME_RATE, (int32_t) frameRate);
  AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL);
  auto bitrate = (int32_t) (width * height * DEFAULT_BITRATE_SCALE);
  AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
  AMediaFormat_setInt32(mediaFormat, "bitrate-mode", MediaCodecUtils::BITRATE_MODE_CBR);
  if (MediaCodecUtils::getApiLevel() >= __ANDROID_API_Q__) {
    AMediaFormat_setInt32(mediaFormat, "max-bframes", 0);
  }

  if (isMtkCpu()) {
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,
      MediaCodecUtils::COLOR_FormatYUV420Planar);
  } else {
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,
      MediaCodecUtils::COLOR_FormatYUV420SemiPlanar);
  }

  if (__builtin_available(android __ANDROID_API_O__, *)) {
    // if we used surface for input, use COLOR_FormatSurface, otherwise use
    // COLOR_FormatYUV420Flexible
    if (surfaceHolder != nullptr) {
      AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,
        MediaCodecUtils::COLOR_FormatSurface);
    }
  }

  videoCodec = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>(
    AMediaCodec_createEncoderByType(mimeType.c_str()));
  AMediaCodec_configure(videoCodec.get(), mediaFormat, nullptr, nullptr,
    AMEDIACODEC_CONFIGURE_FLAG_ENCODE);

  // checks whether we encode video frame from surface
  if (surfaceHolder != nullptr) {
    if (__builtin_available(android __ANDROID_API_O__, *)) {
      ANativeWindow* anw;
      AMediaCodec_createInputSurface(videoCodec.get(), &anw);
      surfaceHolder->surface = std::make_shared<NativeSurface>(anw);
    }
  }
  AMediaFormat_delete(mediaFormat);

  inputEof = false;
  outputEof = false;
  eofDeadline = {};

  return true;
}

void NativeVideoEncoder::start() {
  if (videoCodec) {
    AMediaCodec_start(videoCodec.get());
  }
}

void NativeVideoEncoder::stop() {
  if (videoCodec) {
    AMediaCodec_stop(videoCodec.get());
  }
}

std::shared_ptr<AVPixelBuffer> NativeVideoEncoder::dequeueInputBuffer() {
  static const std::shared_ptr<AVPixelBuffer> sEmptyBuffer = nullptr;
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !videoCodec || surfaceHolder != nullptr) {
    return sEmptyBuffer;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = (int) AMediaCodec_dequeueInputBuffer(videoCodec.get(), TIME_OUT_US);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? sampleBuffer : sEmptyBuffer;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void NativeVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  if (!videoCodec || !inputBuffer) {
    return;
  }
  assert(inputBuffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (inputBuffer->isEndOfStream()) {
    if (__builtin_available(android __ANDROID_API_O__, *)) {
      if (surfaceHolder) {
        AMediaCodec_signalEndOfInputStream(videoCodec.get());
      } else {
        AMediaCodec_queueInputBuffer(videoCodec.get(), inputIndex, 0, 0, 0,
          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      }
    } else {
      AMediaCodec_queueInputBuffer(videoCodec.get(), inputIndex, 0, 0, 0,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    }
    inputIndex = INDEX_UNSET;
    inputEof = true;
    eofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  } else if (inputBuffer->isDiscard()) {
    MINILOG_DEBUG("discard input video frames: {}", inputBuffer->timeUs());
  } else if (inputBuffer->getFormat() == PixelFormat::kFormatSurface) {
    // Do nothing
  } else {
    size_t bufSize = 0;
    auto yuvBuffer = inputBuffer->asI420PixelBuffer();
    if (yuvBuffer != nullptr) {
      auto input = AMediaCodec_getInputBuffer(videoCodec.get(), inputIndex, &bufSize);
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
        auto input = AMediaCodec_getInputBuffer(videoCodec.get(), inputIndex, &bufSize);
        bufSize = std::min(nv12Buffer->size(), bufSize);
        if (isMtkCpu()) {
          MediaCodecUtils::NV12toYUV420Planar(
            nv12Buffer->getData(), 0, input, nv12Buffer->width(), nv12Buffer->height());
        } else {
          memcpy(input, nv12Buffer->getData(), bufSize);
        }
      }
    }
    AMediaCodec_queueInputBuffer(
      videoCodec.get(), inputIndex, 0, bufSize, inputBuffer->timeUs(), 0);
    inputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVPacketBuffer> NativeVideoEncoder::dequeueOutputBuffer() {
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

  AMediaCodecBufferInfo info;
  while (!outputEof) {
    auto outputIndex = AMediaCodec_dequeueOutputBuffer(videoCodec.get(), &info, 0);
    if (outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      break;
    } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer:: AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED");
      auto format = AMediaCodec_getOutputFormat(videoCodec.get());
      if (format != nullptr) {
        outputFormat = MediaCodecUtils::convertToMediaFormat(format);
      }
      AMediaFormat_delete(format);
      queuedOutputBuffer = packetBuffer;
      queuedOutputBuffer->setFormat(getOutputFormat());
      break;
    } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer:: AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED");
    } else if (outputIndex < 0) {
      MINILOG_DEBUG("dequeue output buffer is unavailable, unknown error: {}", outputIndex);
    } else {
      queuedOutputBuffer = packetBuffer;
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        outputEof = true;
        queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
      } else {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
          queuedOutputBuffer->setFlags(AVBufferFlags::kCodecConfig);
        } else if (info.flags & MediaCodecUtils::BUFFER_FLAG_KEY_FRAME) {
          queuedOutputBuffer->setFlags(AVBufferFlags::kKeyFrame);
        }
        size_t bufSize;
        auto encodeData = AMediaCodec_getOutputBuffer(videoCodec.get(), outputIndex, &bufSize);
        if (encodeData) {
          if (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) {
            queuedOutputBuffer->addFlag(AVBufferFlags::kKeyFrame);
          }
          queuedOutputBuffer->timeUs(info.presentationTimeUs);
          queuedOutputBuffer->ensureSpaceForWrite(info.size);
          auto byteBuffer = queuedOutputBuffer->data();
          byteBuffer->position(0);
          byteBuffer->put(encodeData + info.offset, info.size);
          byteBuffer->flip();
        } else {
          queuedOutputBuffer->shouldBeSkipped = true;
        }
      }
      AMediaCodec_releaseOutputBuffer(videoCodec.get(), outputIndex, false);
      break;
    }
  }

  // The codec may still be holding packets when the input's end was signalled; an empty dequeue at
  // that moment is not the end of the output. Only after the deadline, or when the codec itself sent
  // the EOS flag above, is the stream reported as ended.
  if (!queuedOutputBuffer && inputEof
      && (eofDeadline == std::chrono::steady_clock::time_point{}
          || std::chrono::steady_clock::now() >= eofDeadline)) {
    outputEof = true;
    queuedOutputBuffer = packetBuffer;
    queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  }

  return queuedOutputBuffer;
}

void NativeVideoEncoder::releaseOutputBuffer(std::shared_ptr<AVPacketBuffer> const &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void NativeVideoEncoder::signalEndOfInputStream() {
  if (!videoCodec) {
    inputEof = true;
    return;
  }
  if (__builtin_available(android __ANDROID_API_O__, *)) {
    if (surfaceHolder) {
      AMediaCodec_signalEndOfInputStream(videoCodec.get());
    } else {
      AMediaCodec_queueInputBuffer(videoCodec.get(), inputIndex, 0, 0, 0,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    }
    inputIndex = INDEX_UNSET;
    inputEof = true;
    eofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  } else {
    auto buffer = dequeueInputBuffer();
    if (buffer != nullptr) {
      buffer->clear();
      buffer->setFlags(AVBufferFlags::kEndOfStream);
      queueInputBuffer(buffer);
    } else {
      inputEof = true;
      eofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
  }
}

void NativeVideoEncoder::flush() {
  if (videoCodec != nullptr) {
    AMediaCodec_flush(videoCodec.get());
  }
  inputEof = false;
  outputEof = false;
  eofDeadline = {};
  inputIndex = INDEX_UNSET;
}

void NativeVideoEncoder::release() {
  if (videoCodec) {
    AMediaCodec_flush(videoCodec.get());
    AMediaCodec_stop(videoCodec.get());
  }
  videoCodec.reset();
  inputEof = true;
  outputEof = true;
  inputIndex = INDEX_UNSET;
}

} // namespace coral
