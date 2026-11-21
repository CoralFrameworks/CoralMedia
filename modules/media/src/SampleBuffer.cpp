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

#include <media/SampleBuffer.h>

namespace coral {

AVSampleBuffer::~AVSampleBuffer() {
  owner = nullptr;
  buffer.reset();
}

const std::shared_ptr<AVBlockBuffer>& AVSampleBuffer::init(const int64_t timeUs, uint32_t size) {
  timeUs_ = timeUs;
  if (buffer == nullptr || buffer->capacity() < size) {
    buffer = std::make_shared<AVBlockBuffer>(size);
  }
  buffer->position(0);
  buffer->limit(size);
  return buffer;
}

void AVSampleBuffer::clear() {
  AVFrameBuffer::clear();
  if (buffer != nullptr) {
    buffer->clear();
  }
}

void AVSampleBuffer::ensureSpaceForWrite(const size_t length) {
  const auto currentData = buffer;
  if (currentData == nullptr) {
    buffer = std::make_shared<AVBlockBuffer>(length);
    return;
  }

  // Check whether the current buffer is sufficient.
  const auto capacity = currentData->capacity();
  const auto position = currentData->position();
  const auto requiredCapacity = position + length;
  if (capacity >= requiredCapacity) {
    return;
  }

  // Instantiate a new buffer if possible.
  const auto newBuffer = std::make_shared<AVBlockBuffer>(requiredCapacity);
  if (position > 0) {
    currentData->flip();
    newBuffer->put(currentData);
  }
  // Set the new buffer.
  buffer = newBuffer;
}

void AVSampleBuffer::release() {
  if (owner) {
    owner->recycleBuffer(shared_from_this());
  }
}

} // namespace coral
