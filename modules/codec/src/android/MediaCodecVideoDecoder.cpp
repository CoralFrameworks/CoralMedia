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
#include "MediaCodecVideoDecoder.h"

#include "MediaCodecBuffer.h"
#include "MediaCodecUtils.h"

#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace coral {

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

MediaCodecVideoDecoder::MediaCodecVideoDecoder()
  : mHasOutputSurface(false),
    mSurfaceIsSampled(false),
    mWidth(0),
    mHeight(0),
    mInputIndex(INDEX_UNSET),
    mInputEof(false),
    mOutputEof(false),
    outputStartTimeUs(Definitions::kTimeUnset) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() {
  mMediaCodec.reset();
  mMediaFormat.reset();
}

void MediaCodecVideoDecoder::setOutputStartTimeUs(int64_t newStartTimeUs) {
  this->outputStartTimeUs = newStartTimeUs;
}

bool MediaCodecVideoDecoder::configure(const std::shared_ptr<MediaFormat>& mediaFormat,
  const std::shared_ptr<VideoSurface>& surface) {
  if (!mediaFormat) {
    MINILOG_ERROR("Failed to configure the decoder: Format is null");
    return false;
  }
  auto maxInputBufferSize = mediaFormat->getMaxInputSize();
  packetBuffer->ensureSpaceForWrite(maxInputBufferSize);

  auto mimeType = mediaFormat->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  mWidth = mediaFormat->getWidth();
  mHeight = mediaFormat->getHeight();
  if (mWidth == 0 || mHeight == 0) {
    MINILOG_ERROR("Failed to configure decoder: with or height is unavailable");
    return false;
  }

  auto format = JniMediaFormat::create();
  format->setString(JniMediaFormat::KEY_MIME, mimeType);
  format->setInteger(JniMediaFormat::KEY_WIDTH, (int32_t) mediaFormat->getWidth());
  format->setInteger(JniMediaFormat::KEY_HEIGHT, (int32_t) mediaFormat->getHeight());

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  auto csdBuffers = mediaFormat->csdList();
  if (!csdBuffers.empty()) {
    std::array<std::string, 3> csdKeys = { "csd-0", "csd-1", "csd-2" };
    for (int i = 0; i < csdBuffers.size(); ++i) {
      if (csdBuffers[i].empty()) {
        break;
      }
      format->setByteBuffer(csdKeys[i], csdBuffers[i].data(), csdBuffers[i].size());
    }
  }

  auto surfaceObj = surface != nullptr ? (jobject) surface->getInstance() : nullptr;
  mHasOutputSurface = surfaceObj != nullptr;
  mSurfaceIsSampled = surface != nullptr && surface->isSampledByConsumer();
  mSurfaceFrames = surface != nullptr ? surface->getFrameSource() : nullptr;
  mMediaCodec = JniMediaCodec::createDecoderByType(mimeType);
  mMediaCodec->configure(format.get(), surfaceObj, nullptr, 0);

  mInputIndex = INDEX_UNSET;
  mInputEof = false;
  mOutputEof = false;

  return true;
}

void MediaCodecVideoDecoder::start() {
  if (mMediaCodec) {
    mMediaCodec->start();
  }
}

void MediaCodecVideoDecoder::stop() {
  if (mMediaCodec) {
    mMediaCodec->stop();
  }
}

void MediaCodecVideoDecoder::flush() {
  if (mMediaCodec != nullptr) {
    mMediaCodec->flush();
  }
  mInputEof = false;
  mOutputEof = false;
  mInputIndex = INDEX_UNSET;
  dequeuedInputBuffer = nullptr;
}

void MediaCodecVideoDecoder::release() {
  mInputEof = true;
  mOutputEof = true;
  if (mMediaCodec != nullptr) {
    mMediaCodec->stop();
    mMediaCodec->release();
  }
  mMediaCodec.reset();
  mMediaFormat.reset();
  mOutputFormat.reset();
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

std::shared_ptr<AVPacketBuffer> MediaCodecVideoDecoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (mInputEof || !mMediaCodec) {
    return nullptr;
  }
  if (mInputIndex == INDEX_UNSET) {
    mInputIndex = mMediaCodec->dequeueInputBuffer(0);
  }
  dequeuedInputBuffer = mInputIndex != INDEX_UNSET ? packetBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void MediaCodecVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  assert(inputBuffer == dequeuedInputBuffer && mInputIndex != INDEX_UNSET);
  if (inputBuffer->isEndOfStream()) {
    mMediaCodec->queueInputBuffer(mInputIndex, 0, 0, 0, JniMediaCodec::BUFFER_FLAG_END_OF_STREAM);
    mInputEof = true;
    mInputIndex = INDEX_UNSET;
  } else if (!inputBuffer->isFormatChanged() && inputBuffer->size() > 0) {
    size_t bufSize;
    auto inBuffer = mMediaCodec->getInputBuffer(mInputIndex, &bufSize);
    bufSize = std::min(inputBuffer->size(), bufSize);
    if (inBuffer) {
      memcpy(inBuffer, inputBuffer->array(), bufSize);
    }
    mMediaCodec->queueInputBuffer(mInputIndex, 0, bufSize, inputBuffer->timeUs(), 0);
    mInputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVPixelBuffer> MediaCodecVideoDecoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPixelBuffer> outputBuffer = nullptr;
  if (!mMediaCodec) {
    return nullptr;
  }

  if (mOutputEof) {
    outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    return outputBuffer;
  }

  JniMediaCodec::BufferInfo info;
  int outputIndex;
  do {
    outputIndex = mMediaCodec->dequeueOutputBuffer(&info, 0);
  } while (outputIndex == JniMediaCodec::INFO_OUTPUT_BUFFERS_CHANGED);
  if (outputIndex == JniMediaCodec::INFO_TRY_AGAIN_LATER) {
    // Do nothing
  } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_FORMAT_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
    mMediaFormat = mMediaCodec->getOutputFormat();
    parseOutputFormat(mMediaFormat);
    mMediaCodec->setVideoScalingMode(JniMediaCodec::VIDEO_SCALING_MODE_SCALE_TO_FIT);
  } else if (outputIndex < 0) {
    MINILOG_DEBUG("dequeue output buffer is unavailable, index:{}", outputIndex);
  } else {
    if (info.flags & JniMediaCodec::BUFFER_FLAG_END_OF_STREAM) {
      mOutputEof = true;
      mMediaCodec->releaseOutputBuffer(outputIndex, false);
      outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
      outputBuffer->clear();
      outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    } else {
      if (info.flags & JniMediaCodec::BUFFER_FLAG_CODEC_CONFIG) {
        mMediaCodec->releaseOutputBuffer(outputIndex, false);
      }
      // whether the video is rendered to the surface or just the software
      if (hasOutputSurface()) {
        outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
        auto frameBuffer = outputBuffer->asMediaCodecBuffer();
        if (frameBuffer) {
          frameBuffer->setIndex(outputIndex);
          // The consumer the frame has to be taken from: the buffer is the decoder's only link between the
          // surface it rendered into and the queue the conversion stage samples.
          frameBuffer->setSurfaceFrameSource(mSurfaceFrames);
        }
        outputBuffer->timeUs(info.presentationTimeUs);
        if (mSurfaceIsSampled) {
          // The consumer samples the surface itself, so the frame has to be in its queue before the consumer
          // can ask for it. Rendering here is the only point that still has the codec's index, and the index
          // is cleared afterwards so that a later release does not hand the same buffer back twice.
          mMediaCodec->releaseOutputBuffer(outputIndex, true);
          if (frameBuffer) {
            frameBuffer->setIndex(INDEX_UNSET);
          }
        }
      } else {
        int colorFormat;
        if (mMediaFormat->getInteger(JniMediaFormat::KEY_COLOR_FORMAT, &colorFormat)) {
          size_t bufSize;
          auto outputBuf = mMediaCodec->getOutputBuffer(outputIndex, &bufSize);
          if (!outputBuf) {
            mMediaCodec->releaseOutputBuffer(outputIndex, false);
          } else {
            outputBuffer = createSoftwareFrameBuffer(
              outputBuf.get(), info.size, outputIndex, info, colorFormat);
          }
        } else {
          mMediaCodec->releaseOutputBuffer(outputIndex, false);
        }
      }
    }
  }

  if (mInputEof && outputBuffer == nullptr) {
    mOutputEof = true;
    outputBuffer = dequeueAvailableOutputBuffer(PixelFormat::kFormatSurface);
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  }

  if (outputBuffer && !isAtLeastOutputStartTimeUs(outputBuffer->timeUs())) {
    outputBuffer->shouldBeSkipped = true;
  }

  return outputBuffer;
}

void MediaCodecVideoDecoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    mInputEof = true;
  }
}

void MediaCodecVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& frameBuffer, const bool render) {
  if (mMediaCodec != nullptr && frameBuffer != nullptr) {
    const auto buffer = frameBuffer->asMediaCodecBuffer();
    if (buffer != nullptr) {
      if (buffer->index() >= 0) {
        mMediaCodec->releaseOutputBuffer(buffer->index(), render);
      }
    }
  }
  frameBuffer->release();
}

void MediaCodecVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> outputBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (outputBuffer) {
    outputBuffer->clear();
    availableOutputBuffers.push_back(std::move(outputBuffer));
  }
}

void MediaCodecVideoDecoder::parseOutputFormat(const std::unique_ptr<JniMediaFormat>& mediaFormat) {
  if (!mediaFormat) {
    return;
  }
  mOutputFormat = MediaCodecUtils::convertToMediaFormat(mediaFormat);
  auto codecName = mMediaCodec->getName();
  int intValue;
  if (mediaFormat->getInteger(JniMediaFormat::KEY_WIDTH, &intValue)) {
    decodeInfo.width = intValue;
  } else {
    decodeInfo.width = (int) mWidth;
  }
  if (mediaFormat->getInteger(JniMediaFormat::KEY_HEIGHT, &intValue)) {
    decodeInfo.height = intValue;
  } else {
    decodeInfo.height = (int) mHeight;
  }
  if (mediaFormat->getInteger("stride", &intValue)) {
    decodeInfo.stride = intValue;
  } else {
    decodeInfo.stride =
      decodeInfo.stride > 0 ? decodeInfo.stride : decodeInfo.width;
  }
  if (mediaFormat->getInteger("slice-height", &intValue)) {
    decodeInfo.sliceHeight = intValue;
  }
  if (!codecName.empty() && strstr(codecName.c_str(), "OMX.Nvidia.")
      && decodeInfo.sliceHeight == 0) {
    decodeInfo.sliceHeight = ALIGN(decodeInfo.height, 16);
  } else if (!codecName.empty() && strstr(codecName.c_str(), "OMX.SEC.avc.dec")) {
    decodeInfo.sliceHeight = (int) mHeight;
    decodeInfo.stride = (int) mWidth;
  } else if (decodeInfo.sliceHeight == 0) {
    decodeInfo.sliceHeight = decodeInfo.height;
  }
  if (mediaFormat->getInteger("crop-top", &intValue)) {
    decodeInfo.cropTop = intValue;
  }
  if (mediaFormat->getInteger("crop-bottom", &intValue)) {
    decodeInfo.cropBottom = intValue;
  }
  if (mediaFormat->getInteger("crop-left", &intValue)) {
    decodeInfo.cropLeft = intValue;
  }
  if (mediaFormat->getInteger("crop-right", &intValue)) {
    decodeInfo.cropRight = intValue;
  }
  if (mediaFormat->getInteger("display-width", &intValue)) {
    decodeInfo.displayWidth = intValue;
  }
  if (mediaFormat->getInteger("display-height", &intValue)) {
    decodeInfo.displayHeight = intValue;
  }
  if (mediaFormat->getInteger("color-range", &intValue)) {
    decodeInfo.colorInfo.colorRange = MediaCodecUtils::getColorRange(intValue);
  }
  if (mediaFormat->getInteger("color-standard", &intValue)) {
    decodeInfo.colorInfo.colorSpace = MediaCodecUtils::getColorSpace(intValue);
  }
  if (mediaFormat->getInteger("color-transfer", &intValue)) {
    decodeInfo.colorInfo.colorTransfer =
      MediaCodecUtils::getColorTransfer(intValue);
  }

  // calculate the display width and height
  int rotationDegrees = 0;
  if (mediaFormat->getInteger("rotation-degrees", &intValue)) {
    rotationDegrees = intValue;
  }
  auto displayWidth =
    decodeInfo.displayWidth == 0 ? decodeInfo.width : decodeInfo.displayWidth;
  auto displayHeight =
    decodeInfo.displayHeight == 0
      ? decodeInfo.height
      : decodeInfo.displayHeight;
  if (rotationDegrees == 90 || rotationDegrees == 270) {
    decodeInfo.displayWidth = displayHeight;
    decodeInfo.displayHeight = displayWidth;
  } else {
    decodeInfo.displayWidth = displayWidth;
    decodeInfo.displayHeight = displayHeight;
  }
}

std::shared_ptr<AVPixelBuffer>
MediaCodecVideoDecoder::createSoftwareFrameBuffer(
  uint8_t* data, int size, int index, const JniMediaCodec::BufferInfo& info, int colorFormat) {
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

  mMediaCodec->releaseOutputBuffer(index, false);
  return nullptr;
}

std::shared_ptr<AVPixelBuffer>
MediaCodecVideoDecoder::createSoftwareBufferCopyYuv420Planar(
  uint8_t* data, int size, int index, const JniMediaCodec::BufferInfo& info) {
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

  mMediaCodec->releaseOutputBuffer(index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
MediaCodecVideoDecoder::createSoftwareBufferCopyYuv420SemiPlanar(
  uint8_t* data, int size, int index, const JniMediaCodec::BufferInfo& info) {
  uint8_t* src = nullptr;
  auto outputFrame = dequeueAvailableOutputBuffer(PixelFormat::kFormatNV12);
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

  mMediaCodec->releaseOutputBuffer(index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
MediaCodecVideoDecoder::createSoftwareBufferCopyYuv420PackedSemiPlanar(
  uint8_t* data, int size, int index, const JniMediaCodec::BufferInfo& info) {
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

  mMediaCodec->releaseOutputBuffer(index, false);
  return outputFrame;
}

std::shared_ptr<AVPixelBuffer>
MediaCodecVideoDecoder::dequeueAvailableOutputBuffer(const PixelFormat& format) {
  std::shared_ptr<AVPixelBuffer> frameBuffer = nullptr;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (!availableOutputBuffers.empty()) {
      frameBuffer = std::move(availableOutputBuffers.front());
      availableOutputBuffers.pop_front();
    }
    if (!frameBuffer) {
      switch (format) {
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
        default: {
          break;
        }
      }
    }
    frameBuffer->clear();
  }
  return frameBuffer;
}

} // namespace coral
