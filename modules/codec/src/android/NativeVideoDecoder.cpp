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

#include "NativeVideoDecoder.h"
#include "MediaCodecBuffer.h"
#include "MediaCodecUtils.h"

#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <android/native_window_jni.h>

#include <unistd.h>
#include <array>
#include <atomic>
#include <string>
#include <utility>

namespace coral {

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

NativeVideoDecoder::NativeVideoDecoder()
  : mWidth(0),
    mHeight(0),
    mInputIndex(INDEX_UNSET),
    mInputEof(false),
    mOutputEof(false),
    outputStartTimeUs(Definitions::kTimeUnset) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

NativeVideoDecoder::~NativeVideoDecoder() {
  mNativeWindow.reset();
  decoder.reset();
}

bool NativeVideoDecoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the decoder: Format is null");
    return false;
  }
  auto maxInputBufferSize = format->getMaxInputSize();
  packetBuffer->ensureSpaceForWrite(maxInputBufferSize);

  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  mWidth = format->getWidth();
  mHeight = format->getHeight();

  codecFormat = std::unique_ptr<AMediaFormat, AMediaFormatDeleter>(AMediaFormat_new());
  AMediaFormat_setString(codecFormat.get(), AMEDIAFORMAT_KEY_MIME, mimeType.c_str());
  AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_WIDTH, (int32_t) mWidth);
  AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_HEIGHT, (int32_t) mHeight);
  AMediaFormat_setInt32(codecFormat.get(), "rotation-degrees", format->getRotation());

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  auto csdBuffers = format->csdList();
  if (!csdBuffers.empty()) {
    std::array<std::string, 3> csdKeys = { "csd-0", "csd-1", "csd-2" };
    for (int i = 0; i < csdBuffers.size(); ++i) {
      if (csdBuffers[i].empty()) {
        break;
      }
      AMediaFormat_setBuffer(
        codecFormat.get(), csdKeys[i].c_str(), csdBuffers[i].data(), csdBuffers[i].size());
    }
  }

  if (surface != nullptr) {
    // The size has to be known before the codec is configured: a `SurfaceTexture` with no default size is
    // rejected by some implementations (`configure failed with err -22`).
    surface->setDefaultBufferSize(mWidth, mHeight);
    // `getEnvironment()` is the *main* thread's environment, and this runs on the decoding thread: a `JNIEnv`
    // is only valid on the thread it belongs to, so the thread's own is taken here (which attaches it if the
    // decoder thread is not attached yet).
    auto* env = JvmEnv::getThreadEnvironment();
    if (env != nullptr) {
      mNativeWindow = std::unique_ptr<ANativeWindow, NativeWindowDeleter>(
        ANativeWindow_fromSurface(env, static_cast<jobject>(surface->getInstance())));
    }
    // A consumer that samples the surface itself needs every frame queued, which is the difference between a
    // `SurfaceTexture` and a surface the platform presents.
    mSurfaceIsSampled = surface->isSampledByConsumer();
    mSurfaceFrames = surface->getFrameSource();
  }

  decoder = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>(
    AMediaCodec_createDecoderByType(mimeType.c_str()));
  if (decoder == nullptr) {
    MINILOG_ERROR("Failed to create a decoder for {}", mimeType);
    return false;
  }
  auto status = AMediaCodec_configure(decoder.get(), codecFormat.get(),
    mNativeWindow != nullptr ? mNativeWindow.get() : nullptr, nullptr, 0);
  if (status != AMEDIA_OK) {
    // A re-opened page configures a decoder while the previous session's codec may still be releasing its
    // buffers; the platform reports `err -22` and the decoders then produce nothing, which is the black page
    // that used to need a process kill. Reset and give it one more try before giving up.
    MINILOG_WARN("Failed to configure the decoder ({}), retrying with a new codec", static_cast<int>(status));
    usleep(100 * 1000);
    decoder.reset(AMediaCodec_createDecoderByType(mimeType.c_str()));
    if (decoder == nullptr) {
      MINILOG_ERROR("Failed to create a decoder for {} on the retry", mimeType);
      return false;
    }
    status = AMediaCodec_configure(decoder.get(), codecFormat.get(),
      mNativeWindow != nullptr ? mNativeWindow.get() : nullptr, nullptr, 0);
  }
  if (status != AMEDIA_OK) {
    const char* text = AMediaFormat_toString(codecFormat.get());
    MINILOG_ERROR("Failed to configure the decoder: {} (format {})", static_cast<int>(status),
      text != nullptr ? text : "?");
    return false;
  }

  mInputIndex = INDEX_UNSET;
  mInputEof = false;
  mOutputEof = false;

  return true;
}

void NativeVideoDecoder::setOutputStartTimeUs(int64_t newStartTimeUs) {
  outputStartTimeUs = newStartTimeUs;
}

void NativeVideoDecoder::start() {
  if (decoder) {
    const auto status = AMediaCodec_start(decoder.get());
    if (status != AMEDIA_OK) {
      MINILOG_ERROR("Failed to start the decoder: {}", static_cast<int>(status));
    }
  }
}

void NativeVideoDecoder::stop() {
  if (decoder) {
    AMediaCodec_stop(decoder.get());
  }
}

void NativeVideoDecoder::flush() {
  if (decoder) {
    AMediaCodec_flush(decoder.get());
  }
  mInputEof = false;
  mOutputEof = false;
  mInputIndex = INDEX_UNSET;
  dequeuedInputBuffer = nullptr;
}

void NativeVideoDecoder::release() {
  decoder.reset();
  codecFormat.reset();
  mInputEof = true;
  mOutputEof = true;
  mInputIndex = INDEX_UNSET;
  dequeuedInputBuffer = nullptr;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    while (!availableOutputBuffers.empty()) {
      availableOutputBuffers.pop_front();
    }
  }
}

std::shared_ptr<AVPacketBuffer> NativeVideoDecoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (mInputEof || !decoder) {
    return nullptr;
  }
  if (mInputIndex == INDEX_UNSET) {
    mInputIndex = AMediaCodec_dequeueInputBuffer(decoder.get(), 0);
  }
  dequeuedInputBuffer = mInputIndex != INDEX_UNSET ? packetBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void NativeVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> buffer) {
  assert(buffer == dequeuedInputBuffer && mInputIndex != INDEX_UNSET);
  if (!buffer || buffer->isEndOfStream()) {
    AMediaCodec_queueInputBuffer(decoder.get(), mInputIndex, 0, 0, 0,
      AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    mInputIndex = INDEX_UNSET;
    mInputEof = true;
  } else if (!buffer->isFormatChanged() && buffer->size() > 0) {
    size_t bufSize = 0;
    auto inBuffer = AMediaCodec_getInputBuffer(decoder.get(), mInputIndex, &bufSize);
    if (inBuffer == nullptr) {
      MINILOG_ERROR("Failed to get the decoder's input buffer {}", mInputIndex.load());
      mInputIndex = INDEX_UNSET;
      dequeuedInputBuffer = nullptr;
      return;
    }
    bufSize = std::min(buffer->size(), bufSize);
    memcpy(inBuffer, buffer->array(), bufSize);
    const auto status = AMediaCodec_queueInputBuffer(decoder.get(), mInputIndex, 0, bufSize,
      buffer->timeUs(), 0);
    if (status != AMEDIA_OK) {
      MINILOG_ERROR("Failed to queue an input buffer: {}", static_cast<int>(status));
    }
    mInputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVPixelBuffer> NativeVideoDecoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPixelBuffer> outputBuffer = nullptr;
  if (!decoder) {
    return nullptr;
  }

  if (mOutputEof) {
    outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    return outputBuffer;
  }

  AMediaCodecBufferInfo info;
  ssize_t outputIndex;
  do {
    outputIndex = AMediaCodec_dequeueOutputBuffer(decoder.get(), &info, 0);
  } while (outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED);
  if (outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
  } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
    auto mediaFormat = AMediaCodec_getOutputFormat(decoder.get());
    if (mediaFormat != nullptr) {
      parseOutputFormat(mediaFormat);
      codecFormat = std::unique_ptr<AMediaFormat, AMediaFormatDeleter>(mediaFormat);
    }
  } else if (outputIndex < 0) {
    MINILOG_DEBUG("dequeue output buffer is unavailable, index: {}", outputIndex);
  } else {
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      mOutputEof = true;
      AMediaCodec_releaseOutputBuffer(decoder.get(), outputIndex, false);
      outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
      outputBuffer->clear();
      outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    } else {
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
        AMediaCodec_releaseOutputBuffer(decoder.get(), outputIndex, false);
      }
      // whether the video is rendered to the surface or just the software
      if (hasOutputSurface()) {
        outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
        auto frameBuffer = outputBuffer->asMediaCodecBuffer();
        if (frameBuffer) {
          frameBuffer->setIndex(outputIndex);
          frameBuffer->setSurfaceFrameSource(mSurfaceFrames);
        }
        outputBuffer->timeUs(info.presentationTimeUs);
        if (mSurfaceIsSampled) {
          static std::atomic<int> reported{ 0 };
          if (reported++ < 3) {
            MINILOG_DEBUG("decoder: rendered the surface frame at {} us", info.presentationTimeUs);
          }
          // The consumer samples the surface itself, so the frame has to be in its queue before the consumer
          // can ask for it. Rendering here is the only point that still has the codec's index, and the index
          // is cleared afterwards so that a later release does not hand the same buffer back twice.
          AMediaCodec_releaseOutputBuffer(decoder.get(), outputIndex, true);
          if (frameBuffer) {
            frameBuffer->setIndex(INDEX_UNSET);
          }
        }
      } else {
        int colorFormat;
        if (AMediaFormat_getInt32(codecFormat.get(), AMEDIAFORMAT_KEY_COLOR_FORMAT, &colorFormat)) {
          size_t bufSize;
          auto outputBuf = AMediaCodec_getOutputBuffer(decoder.get(), outputIndex, &bufSize);
          if (!outputBuf) {
            AMediaCodec_releaseOutputBuffer(decoder.get(), outputIndex, false);
          } else {
            outputBuffer = createSoftwareFrameBuffer(
              outputBuf, info.size, outputIndex, info, colorFormat);
          }
        } else {
          AMediaCodec_releaseOutputBuffer(decoder.get(), outputIndex, false);
        }
      }
    }
  }

  if (outputBuffer == nullptr && mInputEof) {
    mOutputEof = true;
    outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  }

  return outputBuffer;
}

void NativeVideoDecoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    mInputEof = true;
  }
}

void NativeVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& frameBuffer, bool render) {
  if (decoder != nullptr) {
    const auto buffer = frameBuffer->asMediaCodecBuffer();
    if (buffer != nullptr) {
      AMediaCodec_releaseOutputBuffer(decoder.get(), buffer->index(), render);
    }
  }
  frameBuffer->release();
}

void NativeVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> outputBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (outputBuffer) {
    outputBuffer->clear();
    availableOutputBuffers.push_back(std::move(outputBuffer));
  }
}

void NativeVideoDecoder::parseOutputFormat(AMediaFormat* mediaFormat) {
  int intValue;
  if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, &intValue)) {
    decodeInfo.width = intValue;
  } else {
    decodeInfo.width = mWidth;
  }
  if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, &intValue)) {
    decodeInfo.height = intValue;
  } else {
    decodeInfo.height = mHeight;
  }
  if (AMediaFormat_getInt32(mediaFormat, "stride", &intValue)) {
    decodeInfo.stride = intValue;
  } else {
    decodeInfo.stride = decodeInfo.stride > 0 ? decodeInfo.stride : decodeInfo.width;
  }
  if (AMediaFormat_getInt32(mediaFormat, "slice-height", &intValue)) {
    decodeInfo.sliceHeight = intValue;
  }

  if (__builtin_available(android __ANDROID_API_P__, *)) {
    char* codecName = nullptr;
    AMediaCodec_getName(decoder.get(), &codecName);
    if (!codecName && strstr(codecName, "OMX.Nvidia.") && decodeInfo.sliceHeight == 0) {
      decodeInfo.sliceHeight = ALIGN(decodeInfo.height, 16);
    } else if (!codecName && strstr(codecName, "OMX.SEC.avc.dec")) {
      decodeInfo.sliceHeight = mHeight;
      decodeInfo.stride = mWidth;
    } else if (decodeInfo.sliceHeight == 0) {
      decodeInfo.sliceHeight = decodeInfo.height;
    }
    if (codecName != nullptr) {
      AMediaCodec_releaseName(decoder.get(), codecName);
    }
  }

  if (AMediaFormat_getInt32(mediaFormat, "crop-top", &intValue)) {
    decodeInfo.cropTop = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "crop-bottom", &intValue)) {
    decodeInfo.cropBottom = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "crop-left", &intValue)) {
    decodeInfo.cropLeft = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "crop-right", &intValue)) {
    decodeInfo.cropRight = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "display-width", &intValue)) {
    decodeInfo.displayWidth = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "display-height", &intValue)) {
    decodeInfo.displayHeight = intValue;
  }
  if (AMediaFormat_getInt32(mediaFormat, "color-range", &intValue)) {
    decodeInfo.colorInfo.colorRange = MediaCodecUtils::getColorRange(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, "color-standard", &intValue)) {
    decodeInfo.colorInfo.colorSpace = MediaCodecUtils::getColorSpace(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, "color-transfer", &intValue)) {
    decodeInfo.colorInfo.colorTransfer =
      MediaCodecUtils::getColorTransfer(intValue);
  }

  // calculate the display width and height
  int rotationDegrees = 0;
  if (AMediaFormat_getInt32(mediaFormat, "rotation-degrees", &intValue)) {
    rotationDegrees = intValue;
  }
  auto displayWidth =
    decodeInfo.displayWidth == 0 ? decodeInfo.width : decodeInfo.displayWidth;
  auto displayHeight = decodeInfo.displayHeight == 0
                         ? decodeInfo.height
                         : decodeInfo.displayHeight;
  if (rotationDegrees == 90 || rotationDegrees == 270) {
    decodeInfo.displayWidth = displayHeight;
    decodeInfo.displayHeight = displayWidth;
  } else {
    decodeInfo.displayWidth = displayWidth;
    decodeInfo.displayHeight = displayHeight;
  }
  outputFormat = MediaCodecUtils::convertToMediaFormat(mediaFormat);
}

std::shared_ptr<AVPixelBuffer>
NativeVideoDecoder::createSoftwareFrameBuffer(
  uint8_t* data, int size, ssize_t index, const AMediaCodecBufferInfo& info, int colorFormat) {
  switch (colorFormat) {
    case MediaCodecUtils::COLOR_FormatYUV420Planar: {
      return createSoftwareBufferCopyYuv420Planar(data, size, index, info);
    }
    case MediaCodecUtils::COLOR_FormatYUV420SemiPlanar:
    case MediaCodecUtils::COLOR_QCOM_FormatYUV420SemiPlanar: {
      return createSoftwareBufferCopyYuv420SemiPlanar(data, size, index, info);
    }
    case MediaCodecUtils::COLOR_TI_FormatYUV420PackedSemiPlanar:
    case MediaCodecUtils::COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced: {
      return createSoftwareBufferCopyYuv420PackedSemiPlanar(data, size, index, info);
    }
    default: {
      MINILOG_ERROR("unknown color format: {}", colorFormat);
      break;
    }
  }

  AMediaCodec_releaseOutputBuffer(decoder.get(), index, false);
  return nullptr;
}

std::shared_ptr<AVPixelBuffer>
NativeVideoDecoder::createSoftwareBufferCopyYuv420Planar(
  uint8_t* data, int size, ssize_t index, const AMediaCodecBufferInfo& info) {
  uint8_t* src;
  auto outputFrame = dequeueAvailableOutputBuffer(PixelFormat::kFormatI420);
  outputFrame->timeUs(info.presentationTimeUs);

  auto i420PixelBuffer = outputFrame->asI420PixelBuffer();
  if (i420PixelBuffer) {
    for (int i = 0; i < 3; i++) {
      int stride = decodeInfo.stride;
      int height;

      src = data + info.offset;
      if (i == 0) {
        height = decodeInfo.height;
        src += decodeInfo.cropTop * decodeInfo.stride;
        src += decodeInfo.cropLeft;
      } else {
        height = decodeInfo.height / 2;
        stride = (decodeInfo.stride + 1) / 2;
        src += decodeInfo.sliceHeight * decodeInfo.stride;
        if (i == 2) {
          src += ((decodeInfo.sliceHeight + 1) / 2) * stride;
        }
        src += decodeInfo.cropTop * stride;
        src += (decodeInfo.cropLeft / 2);
      }

      if (i == 0) {
        memcpy(i420PixelBuffer->mutableDataY(), src, height * stride);
      } else if (i == 1) {
        memcpy(i420PixelBuffer->mutableDataU(), src, height * stride);
      } else if (i == 2) {
        memcpy(i420PixelBuffer->mutableDataV(), src, height * stride);
      }
    }
  }

  AMediaCodec_releaseOutputBuffer(decoder.get(), index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
NativeVideoDecoder::createSoftwareBufferCopyYuv420SemiPlanar(
  uint8_t* data, int size, ssize_t index, const AMediaCodecBufferInfo& info) {
  uint8_t* src = nullptr;
  auto outputFrame =
    dequeueAvailableOutputBuffer(PixelFormat::kFormatNV12);
  outputFrame->timeUs(info.presentationTimeUs);

  auto nv12PixelBuffer = outputFrame->asNV12PixelBuffer();
  if (nv12PixelBuffer) {
    for (int i = 0; i < 2; i++) {
      int height = 0;
      src = data + info.offset;
      if (i == 0) {
        height = decodeInfo.height;
        src += decodeInfo.cropTop * decodeInfo.stride;
        src += decodeInfo.cropLeft;
      } else if (i == 1) {
        height = decodeInfo.height / 2;
        src += decodeInfo.sliceHeight * decodeInfo.stride;
        src += decodeInfo.cropTop * decodeInfo.stride;
        src += decodeInfo.cropLeft;
      }

      if (i == 0) {
        memcpy(nv12PixelBuffer->mutableDataY(), src, height * decodeInfo.stride);
      } else {
        memcpy(nv12PixelBuffer->mutableDataUV(), src, height * decodeInfo.stride);
      }
    }
  }

  AMediaCodec_releaseOutputBuffer(decoder.get(), index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
NativeVideoDecoder::createSoftwareBufferCopyYuv420PackedSemiPlanar(
  uint8_t* data, int size, ssize_t index, const AMediaCodecBufferInfo& info) {
  uint8_t* src = nullptr;
  auto outputFrame =
    dequeueAvailableOutputBuffer(PixelFormat::kFormatNV12);
  outputFrame->timeUs(info.presentationTimeUs);

  auto nv12PixelBuffer = outputFrame->asNV12PixelBuffer();
  if (nv12PixelBuffer) {
    for (int i = 0; i < 2; i++) {
      int height;
      src = data + info.offset;
      if (i == 0) {
        height = decodeInfo.height;
      } else if (i == 1) {
        height = decodeInfo.height / 2;
        src +=
          (decodeInfo.sliceHeight - decodeInfo.cropTop / 2) * decodeInfo.stride;
        src += decodeInfo.cropTop * decodeInfo.stride;
        src += decodeInfo.cropLeft;
      }

      if (i == 0) {
        memcpy(nv12PixelBuffer->mutableDataY(), src, height * decodeInfo.stride);
      } else {
        memcpy(nv12PixelBuffer->mutableDataUV(), src, height * decodeInfo.stride);
      }
    }
  }

  AMediaCodec_releaseOutputBuffer(decoder.get(), index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
NativeVideoDecoder::dequeueAvailableOutputBuffer(const PixelFormat& formatType) {
  std::shared_ptr<AVPixelBuffer> frameBuffer = nullptr;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (!availableOutputBuffers.empty()) {
      frameBuffer = std::move(availableOutputBuffers.front());
      availableOutputBuffers.pop_front();
    }
    if (!frameBuffer) {
      switch (formatType) {
        case PixelFormat::kFormatSurface: {
          frameBuffer = std::make_shared<MediaCodecBuffer>(this, mWidth, mHeight);
          break;
        }
        case PixelFormat::kFormatI420: {
          int strideY = decodeInfo.stride;
          int strideU = (decodeInfo.stride + 1) / 2;
          int strideV = (decodeInfo.stride + 1) / 2;
          frameBuffer = std::make_shared<I420PixelBuffer>(
            this, decodeInfo.width, decodeInfo.height, strideY, strideU, strideV);
          break;
        }
        case PixelFormat::kFormatNV12: {
          frameBuffer = std::make_shared<NV12PixelBuffer>(
            this, decodeInfo.width, decodeInfo.height, decodeInfo.stride, decodeInfo.stride);
          break;
        }
      }
    }
    frameBuffer->clear();
  }
  return frameBuffer;
}

} // namespace coral



