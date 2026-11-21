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

#ifndef CORAL_EXTRACTOR_DATA_BUFFER_H
#define CORAL_EXTRACTOR_DATA_BUFFER_H

#include <utils/ByteBuffer.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/**
 * The extractor module counterpart of ExoPlayer's {@code ParsableByteArray}.
 * <p>The class derives from {@link utils::ByteBuffer}, and therefore keeps the mark, position,
 * limit and capacity semantics of a Java NIO buffer, which is what the Java extractors of ExoPlayer
 * are written against. On top of it, this class adds the parsable helpers that the container
 * implementations need, and the ability to grow its backing storage.
 *
 * <p>The buffer owns its storage, therefore copying and moving an instance is safe. Instances are
 * intended to be used as value types inside the parsers, with no lifetime coupling to the extractor
 * that created them.
 */
class AVDataBuffer final : public utils::ByteBuffer {
public:
  AVDataBuffer() noexcept
    : utils::ByteBuffer(static_cast<uint32_t>(kDefaultCapacity)) {
  }

  explicit AVDataBuffer(const uint32_t capacity) noexcept
    : utils::ByteBuffer(capacity) {
  }

  AVDataBuffer(const uint8_t* data, const uint32_t length) noexcept
    : utils::ByteBuffer(length) {
    if (data != nullptr && length > 0) {
      put(data, static_cast<size_t>(length));
      clear();
    }
  }

  explicit AVDataBuffer(const std::vector<uint8_t>& data) noexcept
    : AVDataBuffer(data.empty() ? nullptr : data.data(), static_cast<uint32_t>(data.size())) {
  }

  AVDataBuffer(const AVDataBuffer& other) noexcept
    : utils::ByteBuffer(static_cast<uint32_t>(other.capacity())) {
    copyFrom(other);
  }

  AVDataBuffer& operator=(const AVDataBuffer& other) noexcept {
    if (this != &other) {
      copyFrom(other);
    }
    return *this;
  }

  AVDataBuffer(AVDataBuffer&& other) noexcept
    : utils::ByteBuffer(std::move(other)) {
  }

  AVDataBuffer& operator=(AVDataBuffer&& other) noexcept {
    utils::ByteBuffer::operator=(std::move(other));
    return *this;
  }

  // ================================================================================================
  // Capacity management
  // ================================================================================================

  /**
   * Ensures that the backing storage can hold {@code newCapacity} bytes. The position and the limit
   * are preserved, and the mark is discarded when it becomes invalid.
   */
  void ensureCapacity(const size_t newCapacity) {
    if (newCapacity <= capacity()) {
      return;
    }
    size_t target = capacity() == 0 ? kDefaultCapacity : capacity();
    while (target < newCapacity) {
      target *= 2;
    }
    utils::ByteBuffer grown(static_cast<uint32_t>(target));
    const size_t toCopy = limit() < capacity() ? limit() : capacity();
    if (toCopy > 0) {
      grown.put(array(), toCopy);
    }
    grown.position(position());
    grown.limit(limit());
    utils::ByteBuffer::operator=(std::move(grown));
  }

  /**
   * Ensures that the backing storage can hold {@code capacity} bytes, and sets both the limit and
   * the capacity to that value. The position is reset to zero and the mark is discarded.
   *
   * <p>This is the counterpart of {@code ParsableByteArray.reset(int)}.
   */
  void reset(const size_t capacity) {
    ensureCapacity(capacity);
    clear();
    utils::ByteBuffer::limit(capacity);
  }

  /**
   * Sets the limit of the buffer, growing the backing storage when needed.
   */
  void setLimit(const size_t limit) {
    ensureCapacity(limit);
    utils::ByteBuffer::limit(limit);
  }

  /**
   * Sets the position to {@code position} and the limit to {@code limit}, growing the backing
   * storage when needed. The mark is discarded.
   *
   * <p>This is the counterpart of {@code ParsableByteArray.setPosition(int)} followed by a limit
   * assignment, a combination that the parsers use constantly.
   */
  void truncate(const size_t position, const size_t limit) {
    ensureCapacity(limit);
    utils::ByteBuffer::position(position);
    utils::ByteBuffer::limit(limit);
  }

  // ================================================================================================
  // Big endian reads
  // ================================================================================================

  [[nodiscard]] uint8_t readUnsignedByte() { return get(); }

  [[nodiscard]] int8_t readByte() { return static_cast<int8_t>(get()); }

  [[nodiscard]] uint16_t readUnsignedShort() {
    return static_cast<uint16_t>((static_cast<uint32_t>(get()) << 8) | get());
  }

  [[nodiscard]] int16_t readShort() { return static_cast<int16_t>(readUnsignedShort()); }

  /**
   * Reads a 24 bit big endian unsigned integer.
   */
  [[nodiscard]] uint32_t readUnsignedInt24() {
    return (static_cast<uint32_t>(get()) << 16) | (static_cast<uint32_t>(get()) << 8) | get();
  }

  [[nodiscard]] uint32_t readUnsignedInt() {
    return (static_cast<uint32_t>(get()) << 24) | (static_cast<uint32_t>(get()) << 16) |
           (static_cast<uint32_t>(get()) << 8) | get();
  }

  [[nodiscard]] int32_t readInt() { return static_cast<int32_t>(readUnsignedInt()); }

  [[nodiscard]] uint64_t readUnsignedLong() {
    return (static_cast<uint64_t>(readUnsignedInt()) << 32) | readUnsignedInt();
  }

  [[nodiscard]] int64_t readLong() { return static_cast<int64_t>(readUnsignedLong()); }

  /**
   * Reads a big endian unsigned integer encoded on {@code length} bytes, at most four.
   */
  [[nodiscard]] uint32_t readUnsignedInt(const int length) {
    uint32_t value = 0;
    for (int i = 0; i < length && i < 4; ++i) {
      value = (value << 8) | get();
    }
    return value;
  }

  // ================================================================================================
  // Little endian reads
  // ================================================================================================

  [[nodiscard]] uint16_t readLittleEndianUnsignedShort() {
    return static_cast<uint16_t>(get() | (static_cast<uint32_t>(get()) << 8));
  }

  [[nodiscard]] uint32_t readLittleEndianUnsignedInt24() {
    return static_cast<uint32_t>(get()) | (static_cast<uint32_t>(get()) << 8) |
           (static_cast<uint32_t>(get()) << 16);
  }

  [[nodiscard]] uint32_t readLittleEndianUnsignedInt() {
    return static_cast<uint32_t>(get()) | (static_cast<uint32_t>(get()) << 8) |
           (static_cast<uint32_t>(get()) << 16) | (static_cast<uint32_t>(get()) << 24);
  }

  [[nodiscard]] int32_t readLittleEndianInt() {
    return static_cast<int32_t>(readLittleEndianUnsignedInt());
  }

  [[nodiscard]] uint64_t readLittleEndianUnsignedLong() {
    return static_cast<uint64_t>(readLittleEndianUnsignedInt()) |
           (static_cast<uint64_t>(readLittleEndianUnsignedInt()) << 32);
  }

  // ================================================================================================
  // Bulk reads
  // ================================================================================================

  /**
   * Reads {@code length} bytes into the given vector, resizing it as needed.
   */
  void readBytes(std::vector<uint8_t>& out, const size_t length) {
    out.resize(length);
    const size_t read = copyTo(out.data(), length);
    if (read < length) {
      out.resize(read);
    }
    skipBytes(read);
  }

  /**
   * Returns a copy of the next {@code length} bytes, without advancing the position.
   */
  [[nodiscard]] std::vector<uint8_t> peekBytes(const size_t length) const {
    std::vector<uint8_t> out(length);
    const size_t read = copyTo(out.data(), length);
    if (read < length) {
      out.resize(read);
    }
    return out;
  }

  /**
   * Copies up to {@code length} bytes at the current position to the given destination, without
   * advancing the position.
   *
   * @return The number of bytes that were copied.
   */
  size_t copyTo(uint8_t* destination, size_t length) const {
    const size_t available = remaining();
    if (length > available) {
      length = available;
    }
    if (length > 0) {
      std::memcpy(destination, array() + position(), length);
    }
    return length;
  }

  /**
   * Advances the position by {@code length} bytes, clamped to the limit.
   */
  void skipBytes(const size_t length) {
    const size_t available = remaining();
    position(position() + (length < available ? length : available));
  }

  // ================================================================================================
  // Absolute reads, without changing the position
  // ================================================================================================

  [[nodiscard]] uint32_t getUnsignedInt(const size_t index) const { return getInt(index); }

  [[nodiscard]] uint16_t getUnsignedShort(const size_t index) const { return getShort(index); }

  [[nodiscard]] uint8_t getUnsignedByte(const size_t index) const { return get(index); }

  [[nodiscard]] uint64_t getUnsignedLong(const size_t index) const { return getLong(index); }

  [[nodiscard]] uint16_t getLittleEndianUnsignedShort(const size_t index) const {
    return static_cast<uint16_t>(get(index) | (static_cast<uint32_t>(get(index + 1)) << 8));
  }

  [[nodiscard]] uint32_t getLittleEndianUnsignedInt(const size_t index) const {
    return static_cast<uint32_t>(get(index)) |
           (static_cast<uint32_t>(get(index + 1)) << 8) |
           (static_cast<uint32_t>(get(index + 2)) << 16) |
           (static_cast<uint32_t>(get(index + 3)) << 24);
  }

  /**
   * Returns a human readable description of the buffer state, for diagnostics.
   */
  [[nodiscard]] std::string toString() const {
    return "AVDataBuffer{position=" + std::to_string(position()) + ", limit=" +
           std::to_string(limit()) + ", capacity=" + std::to_string(capacity()) + "}";
  }

private:
  static constexpr size_t kDefaultCapacity = 64;

  void copyFrom(const AVDataBuffer& other) {
    ensureCapacity(other.capacity());
    clear();
    const size_t sourceLimit = other.limit();
    if (sourceLimit > 0) {
      put(other.array(), sourceLimit);
    }
    position(other.position());
    limit(other.limit());
  }
};

} // namespace coral

#endif //CORAL_EXTRACTOR_DATA_BUFFER_H
