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

#include "MediaCodecAudioEncoder.h"

#include "MediaCodecUtils.h"

#include <media/Utilities.h>

#include <core/jni/android/AudioFormat.h>

#include <minilog/log.h>

#include <cassert>

namespace coral {

MediaCodecAudioEncoder::MediaCodecAudioEncoder()
    : inputIndex(INDEX_UNSET),
      inputEof(false),
      outputEof(false),
      presentationTimeUs(0) {
  packetBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
}

bool MediaCodecAudioEncoder::configure(const std::shared_ptr<MediaFormat> &format) {
  if (!format) {
    MINILOG_ERROR("Failed to configure the encoder: Format is null");
    return false;
  }

  auto mimeType = format->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("no available mimeType");
    return false;
  }

  // The max input size must at least no less than one sample frame size
  inputConfig = AudioConfiguration(format->getSampleRate(), format->getChannelCount(), format->getSampleFormat());
  sampleBuffer = std::make_shared<AVSampleBuffer>(inputConfig);

  auto maxInputBufferSize = format->getMaxInputSize();
  maxInputBufferSize = std::max(maxInputBufferSize, DEFAULT_MAX_INPUT_SIZE);
  sampleBuffer->ensureSpaceForWrite(maxInputBufferSize);

  auto mediaFormat = JniMediaFormat::create();
  mediaFormat->setString(JniMediaFormat::KEY_MIME, mimeType);
  mediaFormat->setInteger(JniMediaFormat::KEY_SAMPLE_RATE, inputConfig.sampleRate);
  mediaFormat->setInteger(JniMediaFormat::KEY_CHANNEL_COUNT, inputConfig.channelCount);
  mediaFormat->setInteger(JniMediaFormat::KEY_BIT_RATE, DEFAULT_BIT_RATE);

  // setting pcm-encoding value
  int pcmEncoding;
  switch (inputConfig.sampleFormat) {
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
  mediaFormat->setInteger(JniMediaFormat::KEY_PCM_ENCODING, pcmEncoding);
  mediaFormat->setInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, maxInputBufferSize);

  //!< Sets the default aac profile
  if (format->getAudioFormatType() == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    // MediaCodecInfo.AACObjectLC
    mediaFormat->setInteger(JniMediaFormat::KEY_AAC_PROFILE, 2);
    int channelMask = 1;
    if (inputConfig.channelCount == 2) {
      channelMask = (0x4 | 0x8);
    } else if (inputConfig.channelCount == 1) {
      channelMask = 0x10;
    }
    mediaFormat->setInteger(JniMediaFormat::KEY_CHANNEL_MASK, channelMask);
  }

  audioCodec = JniMediaCodec::createEncoderByType(mimeType);
  audioCodec->configure(mediaFormat.get(), nullptr, nullptr, JniMediaCodec::CONFIGURE_FLAG_ENCODE);

  inputEof = false;
  outputEof = false;
  presentationTimeUs = 0;

  return true;
}

void MediaCodecAudioEncoder::start() {
  if (audioCodec) {
    audioCodec->start();
  }
}

void MediaCodecAudioEncoder::stop() {
  if (audioCodec) {
    audioCodec->stop();
  }
}

std::shared_ptr<AVSampleBuffer> MediaCodecAudioEncoder::dequeueInputBuffer() {
  assert(dequeuedInputBuffer == nullptr);
  if (inputEof || !audioCodec) {
    return nullptr;
  }
  if (inputIndex == INDEX_UNSET) {
    inputIndex = audioCodec->dequeueInputBuffer(TIME_OUT_US);
  }
  dequeuedInputBuffer = inputIndex != INDEX_UNSET ? sampleBuffer : nullptr;
  if (dequeuedInputBuffer) {
    dequeuedInputBuffer->clear();
  }
  return dequeuedInputBuffer;
}

void MediaCodecAudioEncoder::queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) {
  if (!inputBuffer) {
    return;
  }
  assert(inputBuffer == dequeuedInputBuffer && inputIndex != INDEX_UNSET);
  if (inputBuffer->isEndOfStream()) {
    audioCodec->queueInputBuffer(inputIndex, 0, 0, 0, JniMediaCodec::BUFFER_FLAG_END_OF_STREAM);
    inputIndex = INDEX_UNSET;
    inputEof = true;
  } else if (inputBuffer->size() > 0) {
    size_t bufSize;
    auto inBuffer = audioCodec->getInputBuffer(inputIndex, &bufSize);
    if (inBuffer) {
      bufSize = std::min(inputBuffer->size(), bufSize);
      memcpy(inBuffer, inputBuffer->array(), bufSize);
      audioCodec->queueInputBuffer(inputIndex, 0, bufSize, presentationTimeUs, 0);
      presentationTimeUs += MediaUtils::getPcmDurationUs(inputConfig.sampleRate,
          inputConfig.channelCount,
          inputConfig.sampleFormat,
          bufSize);
    }
    inputIndex = INDEX_UNSET;
  }
  dequeuedInputBuffer = nullptr;
}

std::shared_ptr<AVPacketBuffer> MediaCodecAudioEncoder::dequeueOutputBuffer() {
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

  JniMediaCodec::BufferInfo info;
  while (!outputEof) {
    auto outputIndex = audioCodec->dequeueOutputBuffer(&info, 0);
    if (outputIndex == JniMediaCodec::INFO_TRY_AGAIN_LATER) {
      break;
    } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_FORMAT_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_FORMAT_CHANGED.");
      auto format = audioCodec->getOutputFormat();
      if (format != nullptr) {
        outputFormat = MediaCodecUtils::convertToMediaFormat(format);
      }
      queuedOutputBuffer = packetBuffer;
      queuedOutputBuffer->setFormat(getOutputFormat());
      break;
    } else if (outputIndex == JniMediaCodec::INFO_OUTPUT_BUFFERS_CHANGED) {
      MINILOG_DEBUG("dequeueOutputBuffer: INFO_OUTPUT_BUFFERS_CHANGED.");
    } else if (outputIndex < 0) {
      MINILOG_DEBUG("dequeue output buffer is unavailable, unknown error:{}", outputIndex);
    } else {
      queuedOutputBuffer = packetBuffer;
      if (info.flags & JniMediaCodec::BUFFER_FLAG_END_OF_STREAM) {
        outputEof = true;
        queuedOutputBuffer->setFlags(AVBufferFlags::kEndOfStream);
      } else {
        if (info.flags & JniMediaCodec::BUFFER_FLAG_CODEC_CONFIG) {
          queuedOutputBuffer->setFlags(AVBufferFlags::kCodecConfig);
        }
        size_t bufSize;
        auto encodedData = audioCodec->getOutputBuffer(outputIndex, &bufSize);
        if (encodedData) {
          if (info.flags & JniMediaCodec::BUFFER_FLAG_KEY_FRAME) {
            queuedOutputBuffer->addFlag(AVBufferFlags::kKeyFrame);
          }
          queuedOutputBuffer->timeUs(info.presentationTimeUs);
          queuedOutputBuffer->ensureSpaceForWrite(info.size);
          auto outputData = queuedOutputBuffer->data();
          outputData->position(0);
          outputData->put(encodedData.get(), info.offset, info.size);
          outputData->flip();
        } else {
          queuedOutputBuffer->shouldBeSkipped = true;
        }
      }
      audioCodec->releaseOutputBuffer(outputIndex, false);
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

void MediaCodecAudioEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void MediaCodecAudioEncoder::signalEndOfInputStream() {
  auto buffer = dequeueInputBuffer();
  if (buffer != nullptr) {
    buffer->clear();
    buffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(buffer);
  } else {
    inputEof = true;
  }
}

void MediaCodecAudioEncoder::flush() {
  if (audioCodec) {
    audioCodec->flush();
  }
  inputIndex = INDEX_UNSET;
  inputEof = false;
  outputEof = false;
  dequeuedInputBuffer = nullptr;
}

void MediaCodecAudioEncoder::release() {
  if (audioCodec) {
    audioCodec->release();
  }
  audioCodec.reset();
  inputIndex = INDEX_UNSET;
  inputEof = true;
  outputEof = true;
  dequeuedInputBuffer = nullptr;
}

} // namespace coral
