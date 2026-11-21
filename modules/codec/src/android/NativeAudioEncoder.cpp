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

#include "NativeAudioEncoder.h"
#include "MediaCodecUtils.h"

#include <media/Utilities.h>

#include <core/jni/android/AudioFormat.h>

#include <minilog/log.h>

namespace coral {

NativeAudioEncoder::NativeAudioEncoder()
    : inputIndex(INDEX_UNSET),
      inputEof(false),
      outputEof(false),
      presentationTimeUs(0) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
}

bool NativeAudioEncoder::configure(const std::shared_ptr<MediaFormat> &format) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the encoder: Format is null");
    return false;
  }

  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  inputConfig = AudioConfiguration(format->getSampleRate(), format->getChannelCount(), format->getSampleFormat());
  sampleBuffer = std::make_shared<AVSampleBuffer>(inputConfig);

  auto maxInputBufferSize = format->getMaxInputSize();
  maxInputBufferSize = std::max(maxInputBufferSize, DEFAULT_MAX_INPUT_SIZE);
  sampleBuffer->ensureSpaceForWrite(maxInputBufferSize);

  auto bitrate = format->getBitrate() != MediaFormat::NO_VALUE ? format->getBitrate() : DEFAULT_BIT_RATE;

  codecFormat.reset(AMediaFormat_new());
  AMediaFormat_setString(codecFormat.get(), AMEDIAFORMAT_KEY_MIME, mimeType.c_str());
  AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, format->getSampleRate());
  AMediaFormat_setInt32(codecFormat.get(),
      AMEDIAFORMAT_KEY_CHANNEL_COUNT,
      format->getChannelCount());
  AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_BIT_RATE, bitrate);

  // setting pcm-encoding value
  int pcmEncoding;
  switch (inputConfig.sampleFormat) {
    case AudioSampleFormat::kInt16: {
      pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
      break;
    }
    case AudioSampleFormat::kFloat: {
      pcmEncoding = JniAudioFormat::ENCODING_PCM_FLOAT;
      break;
    }
    case AudioSampleFormat::kInt24: {
      pcmEncoding = JniAudioFormat::ENCODING_PCM_24BIT_PACKED;
      break;
    }
    case AudioSampleFormat::kInt32: {
      pcmEncoding = JniAudioFormat::ENCODING_PCM_32BIT;
      break;
    }
    default: {
      pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
      break;
    }
  }

  if (__builtin_available(android __ANDROID_API_P__, *)) {
    AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_PCM_ENCODING, pcmEncoding);
  } else {
    AMediaFormat_setInt32(codecFormat.get(), "pcm-encoding", pcmEncoding);
  }

  AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, maxInputBufferSize);

  //!< Sets the default aac profile
  if (format->getAudioFormatType() == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    // MediaCodecInfo.AACObjectLC
    AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_AAC_PROFILE, 2);
    int channelMask = 1;
    if (inputConfig.channelCount == 2) {
      channelMask = (0x4 | 0x8);
    } else if (inputConfig.channelCount == 1) {
      channelMask = 0x10;
    }
    AMediaFormat_setInt32(codecFormat.get(), AMEDIAFORMAT_KEY_CHANNEL_MASK, channelMask);
  }

  audioCodec = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>(AMediaCodec_createEncoderByType(mimeType.c_str()));
  AMediaCodec_configure(audioCodec.get(), codecFormat.get(), nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  inputEof = false;
  outputEof = false;
  presentationTimeUs = 0;

  return true;
}

void NativeAudioEncoder::start() {
  if (mRunning.load(std::memory_order_acquire)) {
    return;
  }
  if (audioCodec) {
    AMediaCodec_start(audioCodec.get());
    mRunning.store(true, std::memory_order_release);
  }
}

void NativeAudioEncoder::stop() {
  if (!mRunning.load(std::memory_order_acquire)) {
    return;
  }
  if (audioCodec) {
    AMediaCodec_stop(audioCodec.get());
    mRunning.store(false, std::memory_order_release);
  }
}

std::shared_ptr<AVSampleBuffer> NativeAudioEncoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !audioCodec) {
    return nullptr;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = (int)AMediaCodec_dequeueInputBuffer(audioCodec.get(), TIME_OUT_US);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? sampleBuffer : nullptr;
  return dequeuedInputBuffer;
}

void NativeAudioEncoder::queueInputBuffer(const std::shared_ptr<AVSampleBuffer> inputBuffer) {
  if (!inputBuffer) {
    return;
  }
  assert(inputBuffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (inputBuffer->isEndOfStream()) {
    AMediaCodec_queueInputBuffer(audioCodec.get(), inputIndex, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    inputIndex = INDEX_UNSET;
    inputEof = true;
  } else if (inputBuffer->size() > 0) {
    size_t bufSize;
    auto inBuffer = AMediaCodec_getInputBuffer(audioCodec.get(), inputIndex, &bufSize);
    if (inBuffer) {
      auto byteSize = std::min(inputBuffer->size(), bufSize);
      memcpy(inBuffer, inputBuffer->array(), byteSize);
      AMediaCodec_queueInputBuffer(audioCodec.get(), inputIndex, 0, byteSize, presentationTimeUs, 0);
      presentationTimeUs += MediaUtils::getPcmDurationUs(inputConfig.sampleRate,
          inputConfig.channelCount,
          inputConfig.sampleFormat,
          byteSize);
    }
    inputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer.reset();
}

std::shared_ptr<AVPacketBuffer> NativeAudioEncoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPacketBuffer> queuedOutputBuffer = nullptr;
  if (!audioCodec) {
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
    auto outputIndex = AMediaCodec_dequeueOutputBuffer(audioCodec.get(), &info, 0);
    if (outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      break;
    } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
      auto format = AMediaCodec_getOutputFormat(audioCodec.get());
      if (format != nullptr) {
        outputFormat = MediaCodecUtils::convertToMediaFormat(format);
        codecFormat.reset(format);
      }
      queuedOutputBuffer = packetBuffer;
      queuedOutputBuffer->setFormat(getOutputFormat());
      break;
    } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_BUFFERS_CHANGED.");
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
        }
        size_t bufSize;
        auto encodedData = AMediaCodec_getOutputBuffer(audioCodec.get(), outputIndex, &bufSize);
        if (encodedData) {
          if (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) {
            queuedOutputBuffer->addFlag(AVBufferFlags::kKeyFrame);
          }
          queuedOutputBuffer->timeUs(info.presentationTimeUs);
          queuedOutputBuffer->ensureSpaceForWrite(info.size);
          auto outputData = queuedOutputBuffer->data();
          outputData->position(0);
          outputData->put(encodedData + info.offset, info.size);
          outputData->flip();
        } else {
          queuedOutputBuffer->shouldBeSkipped = true;
        }
      }
      AMediaCodec_releaseOutputBuffer(audioCodec.get(), outputIndex, false);
      break;
    }
  }

  // if failed to drain buffer from AMediaCodec and input is end, return an end of stream buffer.
  if (!queuedOutputBuffer && inputEof) {
    queuedOutputBuffer = packetBuffer;
    queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    outputEof = true;
  }

  return queuedOutputBuffer;
}

void NativeAudioEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void NativeAudioEncoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    inputEof = true;
  }
}

void NativeAudioEncoder::flush() {
  if (audioCodec) {
    AMediaCodec_flush(audioCodec.get());
  }
  inputIndex = INDEX_UNSET;
  inputEof = false;
  outputEof = false;
  dequeuedInputBuffer = nullptr;
}

void NativeAudioEncoder::release() {
  audioCodec.reset();
  inputIndex = INDEX_UNSET;
  inputEof = true;
  outputEof = true;
  dequeuedInputBuffer = nullptr;
}

} // namespace coral
