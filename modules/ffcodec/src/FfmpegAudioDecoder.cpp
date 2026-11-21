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

#include <ffcodec/FfmpegAudioDecoder.h>

namespace coral {

FfmpegAudioDecoder::FfmpegAudioDecoder(int numInputBuffers, int numOutputBuffers, bool outputFloat) noexcept
  : decoder(std::make_unique<SimpleAudioDecoder>(numInputBuffers, numOutputBuffers, outputFloat)) {

}

FfmpegAudioDecoder::~FfmpegAudioDecoder() {
  decoder.reset();
}

bool FfmpegAudioDecoder::configure(std::shared_ptr<MediaFormat> const &format) {
  if (format->isAudio()) {
    decoder->configure(format);
    return true;
  }
  return false;
}

void FfmpegAudioDecoder::start() {
  decoder->start();
}

void FfmpegAudioDecoder::stop() {
  decoder->stop();
}

void FfmpegAudioDecoder::flush() {
  decoder->flush();
}

void FfmpegAudioDecoder::release() {
  decoder->release();
}

std::shared_ptr<AVPacketBuffer> FfmpegAudioDecoder::dequeueInputBuffer() {
  return decoder->dequeueInputBuffer();
}

void FfmpegAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  decoder->queueInputBuffer(std::move(inputBuffer));
}

std::shared_ptr<AVSampleBuffer> FfmpegAudioDecoder::dequeueOutputBuffer() {
  return decoder->dequeueOutputBuffer();
}

void FfmpegAudioDecoder::releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void FfmpegAudioDecoder::signalEndOfInputStream() {
  decoder->signalEndOfInputStream();
}

std::shared_ptr<MediaFormat> const &FfmpegAudioDecoder::getOutputFormat() const {
  return decoder->getOutputFormat();
}

char const *FfmpegAudioDecoder::getName() const {
  return decoder->getName();
}

void FfmpegAudioDecoder::setOutputStartTimeUs(int64_t outputStartTimeUs) {
  decoder->setOutputStartTimeUs(outputStartTimeUs);
}

} // namespace coral
