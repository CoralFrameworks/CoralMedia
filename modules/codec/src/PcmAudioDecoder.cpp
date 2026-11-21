/*
 * Copyright (C) 2026 The Coral Project
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

#include "PcmAudioDecoder.h"

#include <media/Definitions.h>
#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>
#include <media/Types.h>
#include <media/Utilities.h>

namespace coral {

PcmAudioDecoder::~PcmAudioDecoder() {
  release();
}

bool PcmAudioDecoder::configure(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr || format->getSampleRate() <= 0 || format->getChannelCount() <= 0) {
    return false;
  }
  mInputFormat = format;
  mOutputFormat = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW,
    format->getSampleRate(), format->getChannelCount());
  auto sampleFormat = format->getSampleFormat();
  if (sampleFormat == AudioSampleFormat::kInvalid) {
    sampleFormat = AudioSampleFormat::kInt16;
  }
  mOutputFormat->setSampleFormat(sampleFormat);
  mOutputFormat->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);

  const auto bytesPerFrame = MediaUtils::getBytesPerFrame(sampleFormat, format->getChannelCount());
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
  mInputBuffer->ensureSpaceForWrite(bytesPerFrame * 4096);
  return true;
}

void PcmAudioDecoder::start() {
  mInputEndOfStream = false;
}

void PcmAudioDecoder::stop() {}

void PcmAudioDecoder::flush() {
  mOutputBuffers.clear();
  mInputDequeued = false;
  mInputEndOfStream = false;
}

void PcmAudioDecoder::release() {
  mOutputBuffers.clear();
  mInputBuffer.reset();
  mInputDequeued = false;
  mInputEndOfStream = false;
}

std::shared_ptr<AVPacketBuffer> PcmAudioDecoder::dequeueInputBuffer() {
  if (mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void PcmAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  if (!inputBuffer) {
    return;
  }
  mInputDequeued = false;
  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }
  if (inputBuffer->size() <= 0 || mOutputFormat == nullptr) {
    return;
  }

  auto output = std::make_shared<AVSampleBuffer>(
    AudioConfiguration(static_cast<uint64_t>(mOutputFormat->getSampleRate()),
      static_cast<uint32_t>(mOutputFormat->getChannelCount()),
      mOutputFormat->getSampleFormat() == AudioSampleFormat::kInvalid
        ? AudioSampleFormat::kInt16
        : mOutputFormat->getSampleFormat()));
  output->init(inputBuffer->timeUs(), inputBuffer->size());
  const auto data = output->data();
  data->position(0);
  data->put(inputBuffer->array(), static_cast<int>(inputBuffer->size()));
  data->limit(inputBuffer->size());
  if (mOutputStartTimeUs == Definitions::kTimeUnset || output->timeUs() >= mOutputStartTimeUs) {
    mOutputBuffers.push_back(std::move(output));
  }
}

std::shared_ptr<AVSampleBuffer> PcmAudioDecoder::dequeueOutputBuffer() {
  if (!mOutputBuffers.empty()) {
    auto output = mOutputBuffers.front();
    mOutputBuffers.pop_front();
    return output;
  }
  if (mInputEndOfStream) {
    auto output = std::make_shared<AVSampleBuffer>(
      AudioConfiguration(static_cast<uint64_t>(mOutputFormat != nullptr ? mOutputFormat->getSampleRate() : 0),
        static_cast<uint32_t>(mOutputFormat != nullptr ? mOutputFormat->getChannelCount() : 0)));
    output->setFlags(AVBufferFlags::kEndOfStream);
    return output;
  }
  return nullptr;
}

void PcmAudioDecoder::releaseOutputBuffer(
  const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool render) {
  static_cast<void>(render);
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void PcmAudioDecoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

const std::shared_ptr<MediaFormat>& PcmAudioDecoder::getOutputFormat() const {
  return mOutputFormat;
}

const char* PcmAudioDecoder::getName() const {
  return "PcmAudioDecoder";
}

void PcmAudioDecoder::setOutputStartTimeUs(int64_t outputStartTimeUs) {
  mOutputStartTimeUs = outputStartTimeUs;
}

} // namespace coral
