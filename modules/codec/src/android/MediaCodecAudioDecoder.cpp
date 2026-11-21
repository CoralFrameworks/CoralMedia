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

#include "MediaCodecAudioDecoder.h"
#include "MediaCodecUtils.h"

#include <core/jni/android/AudioFormat.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <cassert>

namespace coral {

MediaCodecAudioDecoder::MediaCodecAudioDecoder()
    : inputIndex(INDEX_UNSET),
      inputEof(false),
      outputEof(false),
      outputStartTimeUs(Definitions::kTimeUnset) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
}

void MediaCodecAudioDecoder::setOutputStartTimeUs(int64_t newStartTimeUs) {
  this->outputStartTimeUs = newStartTimeUs;
}

bool MediaCodecAudioDecoder::configure(const std::shared_ptr<MediaFormat> &format) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the decoder: Format is null");
    return false;
  }

  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  inputConfig.sampleRate = format->getSampleRate();
  inputConfig.channelCount = format->getChannelCount();
  inputConfig.sampleFormat = format->getSampleFormat();

  // The decoded PCM configuration is what the audio renderer uses to open the device. The output
  // format change below refines it (AAC HE/SBR can change the sample rate), but the buffer must
  // already carry a valid configuration if a decoder emits data without a format callback.
  outputConfig.sampleRate = format->getSampleRate();
  outputConfig.channelCount = format->getChannelCount();
  outputConfig.sampleFormat = format->getSampleFormat();
  if (outputConfig.sampleFormat == AudioSampleFormat::kInvalid ||
      outputConfig.sampleFormat == AudioSampleFormat::kDefault) {
    outputConfig.sampleFormat = AudioSampleFormat::kInt16;
  }

  auto bufferSize = MediaUtils::getPcmBufferSize(nullptr,
      format->getChannelCount(),
      format->getSampleRate(),
      format->getSampleFormat(),
      0);
  bufferSize = std::max(format->getMaxInputSize(), bufferSize);
  packetBuffer->ensureSpaceForWrite(bufferSize);

  auto codecFormat = JniMediaFormat::create();
  codecFormat->setString(JniMediaFormat::KEY_MIME, mimeType);
  codecFormat->setInteger(JniMediaFormat::KEY_SAMPLE_RATE, format->getSampleRate());
  codecFormat->setInteger(JniMediaFormat::KEY_CHANNEL_COUNT, format->getChannelCount());
  auto bitRate = format->getBitrate() != MediaFormat::NO_VALUE ? format->getBitrate() : DEFAULT_BIT_RATE;
  codecFormat->setInteger(JniMediaFormat::KEY_BIT_RATE, bitRate);
  codecFormat->setInteger(JniMediaFormat::KEY_IS_ADTS, 0);

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
  codecFormat->setInteger(JniMediaFormat::KEY_PCM_ENCODING, pcmEncoding);
  codecFormat->setInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, bufferSize);

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  auto csdList = format->csdList();
  if (!csdList.empty()) {
    std::string csdKeys[3] = {"csd-0", "csd-1", "csd-2"};
    for (int i = 0; i < csdList.size(); ++i) {
      if (csdList[i].empty()) {
        break;
      }
      codecFormat->setByteBuffer(csdKeys[i], csdList[i].data(), csdList[i].size());
    }
  }

  audioCodec = JniMediaCodec::createDecoderByType(mimeType);
  audioCodec->configure(codecFormat.get(), nullptr, nullptr, 0);

  inputEof = false;
  outputEof = false;

  return true;
}

void MediaCodecAudioDecoder::start() {
  if (audioCodec) {
    audioCodec->start();
  }
}

void MediaCodecAudioDecoder::stop() {
  if (audioCodec) {
    audioCodec->stop();
  }
}

void MediaCodecAudioDecoder::flush() {
  if (audioCodec) {
    audioCodec->flush();
  }
  inputIndex = INDEX_UNSET;
  inputEof = false;
  outputEof = false;
  dequeuedInputBuffer = nullptr;
}

void MediaCodecAudioDecoder::release() {
  if (audioCodec) {
    audioCodec->release();
  }
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

std::shared_ptr<AVPacketBuffer> MediaCodecAudioDecoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !audioCodec) {
    return nullptr;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = audioCodec->dequeueInputBuffer(0);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? packetBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void MediaCodecAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  assert(inputBuffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (!inputBuffer || inputBuffer->isEndOfStream()) {
    audioCodec->queueInputBuffer(inputIndex, 0, 0, 0, JniMediaCodec::BUFFER_FLAG_END_OF_STREAM);
    inputIndex = INDEX_UNSET;
    inputEof = true;
  } else if (!inputBuffer->isFormatChanged() && inputBuffer->size() > 0) {
    size_t bufSize;
    auto inBuffer = audioCodec->getInputBuffer(inputIndex, &bufSize);
    if (inBuffer) {
      bufSize = std::min(inputBuffer->size(), bufSize);
      memcpy(inBuffer, inputBuffer->array(), bufSize);
    }
    audioCodec->queueInputBuffer(inputIndex, 0, bufSize, inputBuffer->timeUs(), 0);
    inputIndex = INDEX_UNSET;
  } else if (inputBuffer->isFormatChanged()) {
    if (inputBuffer->getFormat() != nullptr
        && inputConfig.sampleRate == inputBuffer->getFormat()->getSampleRate()
        && inputConfig.channelCount == inputBuffer->getFormat()->getChannelCount()
        && inputConfig.sampleFormat == inputBuffer->getFormat()->getSampleFormat()) {
      audioCodec->flush();
    } else {
      // TODO maybe recreate decoder?
    }
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVSampleBuffer> MediaCodecAudioDecoder::dequeueOutputBuffer() {
  std::shared_ptr<AVSampleBuffer> outputBuffer = nullptr;
  if (!audioCodec) {
    return nullptr;
  }

  if (outputEof) {
    outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    return outputBuffer;
  }

  JniMediaCodec::BufferInfo info;
  auto outputIndex = audioCodec->dequeueOutputBuffer(&info, 0);
  if (outputIndex == JniMediaCodec::INFO_TRY_AGAIN_LATER) {
    // Do nothing
  } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_FORMAT_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
    auto mediaFormat = audioCodec->getOutputFormat();
    if (mediaFormat != nullptr) {
      outputFormat = MediaCodecUtils::convertToMediaFormat(mediaFormat);
      if (outputFormat != nullptr) {
        outputConfig.sampleRate = outputFormat->getSampleRate();
        outputConfig.channelCount = outputFormat->getChannelCount();
        outputConfig.sampleFormat = outputFormat->getSampleFormat();
        if (outputConfig.sampleFormat == AudioSampleFormat::kInvalid ||
            outputConfig.sampleFormat == AudioSampleFormat::kDefault) {
          outputConfig.sampleFormat = AudioSampleFormat::kInt16;
        }
      }
    }
  } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_BUFFERS_CHANGED) {
    MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_BUFFERS_CHANGED.");
  } else if (outputIndex < 0) {
    MINILOG_DEBUG("dequeue output buffer is unavailable, unknown error: {}", outputIndex);
  } else {
    outputBuffer = dequeueAvailableOutputBuffer();
    if (info.flags & JniMediaCodec::BUFFER_FLAG_END_OF_STREAM) {
      outputEof = true;
      audioCodec->releaseOutputBuffer(outputIndex, false);
      outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    } else {
      if (info.flags & JniMediaCodec::BUFFER_FLAG_CODEC_CONFIG) {
        outputBuffer->setFlags(AVBufferFlags::kCodecConfig);
      }
      size_t bufSize;
      auto outputBuf = audioCodec->getOutputBuffer(outputIndex, &bufSize);
      if (outputBuf) {
        outputBuffer->init(info.presentationTimeUs, info.size);
        auto outputData = outputBuffer->data();
        outputData->position(0);
        outputData->put(outputBuf.get(), info.offset, info.size);
        outputData->flip();
      }
      audioCodec->releaseOutputBuffer(outputIndex, false);
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

void MediaCodecAudioDecoder::releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void MediaCodecAudioDecoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    inputEof = true;
  }
}

void MediaCodecAudioDecoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (decodedBuffer) {
    decodedBuffer->clear();
    availableOutputBuffers.push_back(std::move(decodedBuffer));
  }
}

std::shared_ptr<AVSampleBuffer> MediaCodecAudioDecoder::dequeueAvailableOutputBuffer() {
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
