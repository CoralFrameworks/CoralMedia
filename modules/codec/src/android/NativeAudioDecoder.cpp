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

#include "NativeAudioDecoder.h"
#include "MediaCodecUtils.h"

#include <media/Utilities.h>

#include <core/jni/android/AudioFormat.h>

#include <minilog/log.h>

namespace coral {

NativeAudioDecoder::NativeAudioDecoder()
    : inputIndex(INDEX_UNSET),
      inputEof(false),
      outputEof(false),
      outputStartTimeUs(Definitions::kTimeUnset) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
}

void NativeAudioDecoder::setOutputStartTimeUs(int64_t newStartTimeUs) {
  outputStartTimeUs = newStartTimeUs;
}

bool NativeAudioDecoder::configure(const std::shared_ptr<MediaFormat> &format) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the decoder: Format is null");
    return false;
  }

  auto maxInputBufferSize = format->getMaxInputSize();
  auto bufferSize = MediaUtils::getPcmBufferSize(nullptr,
      format->getChannelCount(),
      format->getSampleRate(),
      format->getSampleFormat(),
      0);
  maxInputBufferSize = std::max(maxInputBufferSize, bufferSize);
  packetBuffer->ensureSpaceForWrite(maxInputBufferSize);

  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  auto codecFormat = AMediaFormat_new();
  AMediaFormat_setString(codecFormat, AMEDIAFORMAT_KEY_MIME, mimeType.c_str());
  AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, (int32_t)format->getSampleRate());
  AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, (int32_t)format->getChannelCount());
  auto bitRate = format->getBitrate() != MediaFormat::NO_VALUE
      ? format->getBitrate()
      : DEFAULT_BIT_RATE;
  AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitRate);
  AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_IS_ADTS, 0);

  int pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
  switch (format->getSampleFormat()) {
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
    AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_PCM_ENCODING, pcmEncoding);
  } else {
    AMediaFormat_setInt32(codecFormat, "pcm-encoding", pcmEncoding);
  }

  AMediaFormat_setInt32(codecFormat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, maxInputBufferSize);

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  auto csdList = format->csdList();
  if (!csdList.empty()) {
    std::string csdKeys[3] = {"csd-0", "csd-1", "csd-2"};
    for (int i = 0; i < csdList.size(); ++i) {
      if (csdList[i].empty()) {
        break;
      }
      AMediaFormat_setBuffer(codecFormat, csdKeys[i].c_str(), csdList[i].data(), csdList[i].size());
    }
  }

  audioCodec = std::unique_ptr<AMediaCodec, AMediaCodecDeleter>(
      AMediaCodec_createDecoderByType(mimeType.c_str()));
  AMediaCodec_configure(audioCodec.get(), codecFormat, nullptr, nullptr, 0);
  AMediaFormat_delete(codecFormat);

  inputEof = false;
  outputEof = false;

  return true;
}

void NativeAudioDecoder::start() {
  if (audioCodec) {
    AMediaCodec_start(audioCodec.get());
  }
}

void NativeAudioDecoder::stop() {
  if (audioCodec) {
    AMediaCodec_stop(audioCodec.get());
  }
}

void NativeAudioDecoder::flush() {
  if (audioCodec) {
    AMediaCodec_flush(audioCodec.get());
  }
  inputIndex = INDEX_UNSET;
  inputEof = false;
  outputEof = false;
  dequeuedInputBuffer = nullptr;
}

void NativeAudioDecoder::release() {
  audioCodec.reset();
  inputIndex = INDEX_UNSET;
  inputEof = true;
  outputEof = true;
  dequeuedInputBuffer = nullptr;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    while (!availableOutputBuffers.empty()) {
      availableOutputBuffers.pop_front();
    }
  }
}

std::shared_ptr<AVPacketBuffer> NativeAudioDecoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !audioCodec) {
    return nullptr;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = AMediaCodec_dequeueInputBuffer(audioCodec.get(), 0);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? packetBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void NativeAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> buffer) {
  if (!buffer) {
    return;
  }
  assert(buffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (buffer->isEndOfStream()) {
    AMediaCodec_queueInputBuffer(audioCodec.get(), inputIndex, 0, 0, 0,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    inputIndex = INDEX_UNSET;
    inputEof = true;
  } else if (!buffer->isFormatChanged() && buffer->size() > 0) {
    size_t bufSize;
    auto inBuffer = AMediaCodec_getInputBuffer(audioCodec.get(), inputIndex, &bufSize);
    if (inBuffer) {
      bufSize = std::min(buffer->size(), bufSize);
      memcpy(inBuffer, buffer->array(), bufSize);
    }
    AMediaCodec_queueInputBuffer(audioCodec.get(), inputIndex, 0, bufSize, buffer->timeUs(), 0);
    inputIndex = INDEX_UNSET;
  } else if (buffer->isFormatChanged()) {
    // TODO maybe recreate decoder?
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVSampleBuffer> NativeAudioDecoder::dequeueOutputBuffer() {
  std::shared_ptr<AVSampleBuffer> outputBuffer = nullptr;
  if (!audioCodec) {
    return nullptr;
  }

  if (outputEof) {
    outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    return outputBuffer;
  }

  AMediaCodecBufferInfo info;
  auto outputIndex = AMediaCodec_dequeueOutputBuffer(audioCodec.get(), &info, 0);
  if (outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
    // Do nothing
  } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
    auto mediaFormat = AMediaCodec_getOutputFormat(audioCodec.get());
    if (mediaFormat != nullptr) {
      parseOutputFormat(mediaFormat);
    }
    AMediaFormat_delete(mediaFormat);
  } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_BUFFERS_CHANGED.");
  } else if (outputIndex < 0) {
    MINILOG_DEBUG("dequeue output buffer is unavailable, unknown error: {}", outputIndex);
  } else {
    outputBuffer = dequeueAvailableOutputBuffer();
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      outputEof = true;
      AMediaCodec_releaseOutputBuffer(audioCodec.get(), outputIndex, false);
      outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    } else {
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
        outputBuffer->setFlags(AVBufferFlags::kCodecConfig);
      }
      size_t bufSize;
      auto outputBuf = AMediaCodec_getOutputBuffer(audioCodec.get(), outputIndex, &bufSize);
      if (outputBuf) {
        outputBuffer->init(info.presentationTimeUs, info.size);
        auto outputData = outputBuffer->data();
        outputData->position(0);
        outputData->put(outputBuf, info.offset, info.size);
        outputData->flip();
      }
      AMediaCodec_releaseOutputBuffer(audioCodec.get(), outputIndex, false);
    }
  }

  // if failed to drain buffer from AMediaCodec and input is end, return an end of stream buffer.
  if (!outputBuffer && inputEof) {
    outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    outputEof = true;
  }

  if (outputBuffer && !isAtLeastOutputStartTimeUs(outputBuffer->timeUs())) {
    outputBuffer->shouldBeSkipped = true;
  }

  return outputBuffer;
}

void NativeAudioDecoder::releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void NativeAudioDecoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    inputEof = true;
  }
}

void NativeAudioDecoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (decodedBuffer) {
    decodedBuffer->clear();
    availableOutputBuffers.push_back(std::move(decodedBuffer));
  }
}

void NativeAudioDecoder::parseOutputFormat(AMediaFormat *mediaFormat) {
  outputFormat = MediaCodecUtils::convertToMediaFormat(mediaFormat);
  if (outputFormat) {
    outputConfig = AudioConfiguration(outputFormat->getSampleRate(),
        outputFormat->getChannelCount(),
        outputFormat->getSampleFormat());
  }
}

std::shared_ptr<AVSampleBuffer> NativeAudioDecoder::dequeueAvailableOutputBuffer() {
  std::shared_ptr<AVSampleBuffer> sampleBuffer = nullptr;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (!availableOutputBuffers.empty()) {
      sampleBuffer = std::move(availableOutputBuffers.front());
      sampleBuffer->setConfiguration(outputConfig);
      availableOutputBuffers.pop_front();
    }
    if (!sampleBuffer) {
      sampleBuffer = std::make_shared<AVSampleBuffer>(this, outputConfig);
    }
    sampleBuffer->clear();
  }
  return sampleBuffer;
}

} // namespace coral
