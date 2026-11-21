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

#include <ffcodec/FfmpegVideoDecoder.h>

namespace coral {

FfmpegVideoDecoder::FfmpegVideoDecoder() {
  decoder = std::make_unique<SimpleVideoDecoder>();
}

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
  decoder.reset();
}

bool FfmpegVideoDecoder::configure(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface) {
  if (format->isVideo()) {
    decoder->configure(format);
    return true;
  }
  return false;
}

void FfmpegVideoDecoder::start() {
  decoder->start();
}

void FfmpegVideoDecoder::stop() {
  decoder->stop();
}

void FfmpegVideoDecoder::flush() {
  decoder->flush();
}

void FfmpegVideoDecoder::release() {
  decoder->release();
}

std::shared_ptr<AVPacketBuffer> FfmpegVideoDecoder::dequeueInputBuffer() {
  return decoder->dequeueInputBuffer();
}

void FfmpegVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  decoder->queueInputBuffer(std::move(inputBuffer));
}

std::shared_ptr<AVPixelBuffer> FfmpegVideoDecoder::dequeueOutputBuffer() {
  return decoder->dequeueOutputBuffer();
}

void FfmpegVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer> &frameBuffer, bool render) {
  if (render) {
    // TODO render video ???
  }
  if (frameBuffer) {
    frameBuffer->release();
  }
}

void FfmpegVideoDecoder::signalEndOfInputStream() {
  decoder->signalEndOfInputStream();
}

const std::shared_ptr<MediaFormat>& FfmpegVideoDecoder::getOutputFormat() const {
  return decoder->getOutputFormat();
}

char const* FfmpegVideoDecoder::getName() const {
  return decoder->getName();
}

void FfmpegVideoDecoder::setOutputStartTimeUs(int64_t outputStartTimeUs) {
  decoder->setOutputStartTimeUs(outputStartTimeUs);
}

} // namespace coral
