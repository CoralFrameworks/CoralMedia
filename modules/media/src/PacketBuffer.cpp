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

#include <media/PacketBuffer.h>

#include <memory>

namespace coral {

std::shared_ptr<AVPacketBuffer> AVPacketBuffer::newFlagsOnlyInstance() {
  return std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeInvalid);
}

void AVPacketBuffer::resetSupplementalData(int length) {
  if (mSupplementalData == nullptr || mSupplementalData->capacity() < length) {
    mSupplementalData = std::make_shared<AVBlockBuffer>(length);
  } else {
    mSupplementalData->clear();
  }
}

void AVPacketBuffer::ensureSpaceForWrite(size_t length) {
  length += paddingSize;
  auto currentData = mData;
  if (currentData == nullptr) {
    mData = std::make_shared<AVBlockBuffer>(length);
    return;
  }

  // Check whether the current buffer is sufficient.
  int capacity = currentData->capacity();
  int position = currentData->position();
  int requiredCapacity = position + length;
  if (capacity >= requiredCapacity) {
    return;
  }

  // Instantiate a new buffer if possible.
  auto newData = std::make_shared<AVBlockBuffer>(requiredCapacity);
  if (position > 0) {
    currentData->flip();
    newData->put(currentData);
  }
  // Set the new buffer.
  mData = newData;
}

} // namespace coral
