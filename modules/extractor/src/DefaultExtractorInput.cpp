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

#include <extractor/DefaultExtractorInput.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace coral {

AVDefaultExtractorInput::AVDefaultExtractorInput(std::shared_ptr<AVDataSource> dataSource,
                                                 const long position)
  : dataSource_(std::move(dataSource)),
    peekBuffer_(static_cast<uint32_t>(kDefaultBufferSize)),
    position_(position),
    peekPosition_(position) {
  if (dataSource_ != nullptr) {
    length_ = dataSource_->getLength();
  }
  positionHolder_.setPosition(position);
  // The buffer is invalidated so that the first read loads it.
  invalidatePeekBuffer();
}

bool AVDefaultExtractorInput::setDataSource(const std::shared_ptr<AVDataSource>& dataSource) {
  if (dataSource == nullptr || !dataSource->isOpen()) {
    return false;
  }
  dataSource_ = dataSource;
  length_ = dataSource->getLength();
  invalidatePeekBuffer();
  endOfInput_ = false;
  return true;
}

// ==================================================================================================
// Buffer management
// ==================================================================================================

void AVDefaultExtractorInput::invalidatePeekBuffer() {
  bufferStartPosition_ = kEndOfInput;
  bytesLoaded_ = 0;
}

long AVDefaultExtractorInput::loadPeekBuffer(const long position, const bool forceLoad) {
  if (dataSource_ == nullptr) {
    return 0;
  }
  if (!forceLoad && bufferStartPosition_ == position) {
    return bytesLoaded_;
  }
  if (length_ >= 0 && position >= length_) {
    // The end of the input is known, there is nothing to load.
    bufferStartPosition_ = position;
    bytesLoaded_ = 0;
    return 0;
  }

  peekBuffer_.reset(kDefaultBufferSize);
  const int read = dataSource_->readAt(position, peekBuffer_.array(), 0, kDefaultBufferSize);
  if (read <= 0) {
    bufferStartPosition_ = position;
    bytesLoaded_ = 0;
    endOfInput_ = true;
    return 0;
  }

  bufferStartPosition_ = position;
  bytesLoaded_ = read;
  peekBuffer_.truncate(0, static_cast<size_t>(read));
  if (read < static_cast<int>(kDefaultBufferSize)) {
    endOfInput_ = true;
  }
  return bytesLoaded_;
}

void AVDefaultExtractorInput::advancePosition(const long length) {
  position_ += length;
  peekPosition_ += length;
  positionHolder_.setPosition(position_);
}

long AVDefaultExtractorInput::advanceStep(const long requested) {
  if (requested <= 0) {
    return 0;
  }
  const long loaded = loadPeekBuffer(position_, false);
  if (loaded <= 0) {
    endOfInput_ = true;
    return kEndOfInput;
  }
  const long read = std::min<long>(loaded, requested);
  advancePosition(read);
  if (read < requested) {
    endOfInput_ = true;
  }
  return read;
}

// ==================================================================================================
// Peek methods
// ==================================================================================================

long AVDefaultExtractorInput::peek(uint8_t target[], size_t offset, size_t length) {
  if (length == 0) {
    return 0;
  }
  const long loaded = loadPeekBuffer(peekPosition_, false);
  const long available = bufferStartPosition_ + loaded - peekPosition_;
  if (loaded <= 0 || available <= 0) {
    // The peek position is at, or beyond, the end of the input. It is pinned to the end so that a
    // further peek cannot move it any further.
    peekPosition_ = bufferStartPosition_ + loaded;
    return kEndOfInput;
  }
  const auto toCopy = static_cast<size_t>(std::min<long>(available, static_cast<long>(length)));
  std::memcpy(target + offset, peekBuffer_.array() + (peekPosition_ - bufferStartPosition_), toCopy);
  peekPosition_ += static_cast<long>(toCopy);
  return static_cast<long>(toCopy);
}

int AVDefaultExtractorInput::peekByte() {
  const long loaded = loadPeekBuffer(peekPosition_, false);
  const long available = bufferStartPosition_ + loaded - peekPosition_;
  if (loaded <= 0 || available <= 0) {
    peekPosition_ = bufferStartPosition_ + loaded;
    return static_cast<int>(kEndOfInput);
  }
  const int value = peekBuffer_.get(static_cast<size_t>(peekPosition_ - bufferStartPosition_));
  peekPosition_ += 1;
  return value;
}

void AVDefaultExtractorInput::skipPeeked(const long length) {
  if (length <= 0) {
    return;
  }
  peekPosition_ += length;
}

void AVDefaultExtractorInput::setPeekPosition(const long position) {
  if (position < 0) {
    resetPeekPosition();
    return;
  }
  const long target = position_ + position;
  if (target < peekPosition_) {
    // The peek position moves backwards, the read ahead buffer is discarded and lazily reloaded.
    peekPosition_ = target;
    invalidatePeekBuffer();
    return;
  }
  peekPosition_ = target;
}

void AVDefaultExtractorInput::resetPeekPosition() {
  peekPosition_ = position_;
}

// ==================================================================================================
// Read methods
// ==================================================================================================

long AVDefaultExtractorInput::read(uint8_t target[], size_t offset, size_t length) {
  if (length == 0) {
    return 0;
  }
  // A read always starts at the read position and consumes it.
  resetPeekPosition();
  const long read = advanceStep(static_cast<long>(length));
  if (read <= 0) {
    return kEndOfInput;
  }
  std::memcpy(target + offset, peekBuffer_.array(), static_cast<size_t>(read));
  return read;
}

long AVDefaultExtractorInput::read(AVDataBuffer& buffer, const size_t length) {
  if (length == 0) {
    buffer.reset(0);
    return 0;
  }
  resetPeekPosition();
  const long read = advanceStep(static_cast<long>(length));
  if (read <= 0) {
    buffer.reset(0);
    return kEndOfInput;
  }
  buffer.reset(static_cast<size_t>(read));
  buffer.put(peekBuffer_.array(), static_cast<size_t>(read));
  buffer.truncate(0, static_cast<size_t>(read));
  return read;
}

long AVDefaultExtractorInput::skip(long length) {
  if (length <= 0) {
    return 0;
  }
  resetPeekPosition();
  const long skipped = advanceStep(length);
  return skipped > 0 ? skipped : kEndOfInput;
}

void AVDefaultExtractorInput::skipFully() {
  if (length_ >= 0) {
    const long remaining = length_ - position_;
    if (remaining > 0) {
      skip(remaining);
      return;
    }
  }
  uint8_t scratch[4096];
  while (this->read(scratch, 0, sizeof(scratch)) > 0) {
    // Keep reading until the end of the input is reached.
  }
}

bool AVDefaultExtractorInput::setPosition(const long position, const bool allowEndOfInput) {
  resetPeekPosition();
  if (position == position_) {
    return true;
  }
  if (length_ >= 0 && position > length_) {
    return false;
  }
  if (position < position_) {
    // Seeking backwards restarts the read ahead buffer at the new position.
    position_ = position;
    peekPosition_ = position;
    positionHolder_.setPosition(position);
    invalidatePeekBuffer();
    return true;
  }
  // Moving forward is a skip, and a skip is served from the read ahead buffer, which is smaller than
  // the distance a container may ask to move: the target is only reached after several steps. A single
  // step left the position short of the target while reporting success, so a container went on reading
  // from a place it did not ask for.
  const long distance = position - position_;
  if (!skipFully(distance)) {
    // The end of the input was reached before the requested position, which the caller may allow.
    return allowEndOfInput;
  }
  return true;
}

bool AVDefaultExtractorInput::resetPosition(const long position, const bool allowEndOfInput) {
  return setPosition(position, allowEndOfInput);
}

// ==================================================================================================
// Fully reading methods
// ==================================================================================================

bool AVDefaultExtractorInput::readFully(uint8_t target[], size_t offset, size_t length) {
  size_t totalRead = 0;
  while (totalRead < length) {
    const long read = this->read(target, offset + totalRead, length - totalRead);
    if (read == kEndOfInput) {
      return false;
    }
    totalRead += static_cast<size_t>(read);
  }
  return true;
}

bool AVDefaultExtractorInput::readFully(AVDataBuffer& target, const size_t length) {
  return readFullyImpl(target, length);
}

bool AVDefaultExtractorInput::readFullyImpl(AVDataBuffer& target, const size_t length) {
  target.reset(length);
  if (length == 0) {
    return true;
  }
  size_t totalRead = 0;
  while (totalRead < length) {
    uint8_t* destination = target.array() + totalRead;
    const long read = this->read(destination, 0, length - totalRead);
    if (read == kEndOfInput) {
      target.truncate(0, totalRead);
      return false;
    }
    totalRead += static_cast<size_t>(read);
  }
  target.truncate(0, length);
  return true;
}

bool AVDefaultExtractorInput::peekFully(uint8_t target[], size_t offset, size_t length) {
  size_t totalPeeked = 0;
  while (totalPeeked < length) {
    const long peeked = peek(target, offset + totalPeeked, length - totalPeeked);
    if (peeked == kEndOfInput) {
      return false;
    }
    if (peeked == 0) {
      // Defensive: never spin when the input reports no progress at all.
      return false;
    }
    totalPeeked += static_cast<size_t>(peeked);
  }
  return true;
}

bool AVDefaultExtractorInput::skipFully(const long length) {
  if (length <= 0) {
    return length == 0;
  }
  // The read ahead buffer serves at most its own size per step, and the containers ask to skip far
  // more than that: the payload of an `mdat` box, an unknown chunk of a WAV file, or the padding of a
  // FLAC block. Skipping only the first step and reporting the end of the input made every one of them
  // fail to find what follows the data they skip.
  long skipped = 0;
  while (skipped < length) {
    const long step = skip(length - skipped);
    if (step <= 0 || step == kEndOfInput) {
      // The end of the input was reached before the whole length was skipped.
      return false;
    }
    skipped += step;
  }
  return true;
}

} // namespace coral
