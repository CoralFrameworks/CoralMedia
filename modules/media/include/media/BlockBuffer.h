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

#ifndef CORAL_MEDIA_BLOCK_BUFFER_H
#define CORAL_MEDIA_BLOCK_BUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * A blocking data buffer.
 */
class AVBlockBuffer {
private:
  static constexpr int DEFAULT_MAX_BUFFER_SIZE = 2048;
  static constexpr int BUFFER_SIZE_INCREASE = 2048;

  int32_t mMark;
  size_t mLimit;
  size_t mPosition;
  size_t mCapacity;
  std::vector<uint8_t> mBuffer;

public:
  AVBlockBuffer() : AVBlockBuffer(DEFAULT_MAX_BUFFER_SIZE) {
  }

  explicit AVBlockBuffer(const uint32_t capacity) noexcept
    : mMark(-1),
      mLimit(capacity),
      mPosition(0),
      mCapacity(capacity) {
    mBuffer.resize(capacity);
  }

  explicit AVBlockBuffer(const uint8_t* arr, const uint32_t length) noexcept
    : AVBlockBuffer(length) {
    put(arr, mCapacity);
    clear();
  }

  ~AVBlockBuffer() {
    mBuffer.clear();
  }

  AVBlockBuffer(const AVBlockBuffer& other) = default;
  AVBlockBuffer& operator=(const AVBlockBuffer& other) = default;

  [[nodiscard]] const uint8_t* array() const { return mBuffer.data(); }

  uint8_t* array() { return mBuffer.data(); }

  /**
   * Returns this buffer's capacity.
   *
   * @return  The capacity of this buffer
   */
  [[nodiscard]] size_t capacity() const { return mCapacity; }

  /**
   * Returns this buffer's position.
   *
   * @return  The position of this buffer
   */
  [[nodiscard]] size_t position() const { return mPosition; }

  /**
   * Returns this buffer's limit.
   *
   * @return  The limit of this buffer
   */
  [[nodiscard]] size_t limit() const { return mLimit; }

  /**
   * Sets this buffer's limit.  If the position is larger than the new limit
   * then it is set to the new limit.  If the mark is defined and larger than
   * the new limit then it is discarded.
   *
   * @param  newLimit
   *         The new limit value; must be non-negative
   *         and no larger than this buffer's capacity
   *
   * @return  This buffer
   */
  AVBlockBuffer& limit(const size_t newLimit) {
    mLimit = newLimit;
    if (mPosition > newLimit) {
      mPosition = newLimit;
    }
    if (mMark > newLimit) {
      mMark = -1;
    }
    return *this;
  }

  /**
   * Sets this buffer's position.  If the mark is defined and larger than the
   * new position then it is discarded.
   *
   * @param  newPosition
   *         The new position value; must be non-negative
   *         and no larger than the current limit
   *
   * @return  This buffer
   */
  AVBlockBuffer& position(const size_t newPosition) {
    mPosition = newPosition;
    if (mMark > newPosition) {
      mMark = -1;
    }
    return *this;
  }

  /**
   * Clears this buffer.  The position is set to zero, the limit is set to
   * the capacity, and the mark is discarded.
   *
   * <p> Invoke this method before using a sequence of channel-read or
   * <i>put</i> operations to fill this buffer.  For example:
   *
   * <blockquote><pre>
   * buf.clear();     // Prepare buffer for reading
   * in.read(buf);    // Read data</pre></blockquote>
   *
   * <p> This method does not actually erase the data in the buffer, but it
   * is named as if it did because it will most often be used in situations
   * in which that might as well be the case. </p>
   *
   * @return  This buffer
   */
  AVBlockBuffer& clear() {
    mPosition = 0;
    mMark = -1;
    mLimit = mCapacity;
    return *this;
  }

  /**
   * Flips this buffer.  The limit is set to the current position and then
   * the position is set to zero.  If the mark is defined then it is
   * discarded.
   *
   * <p> After a sequence of channel-read or <i>put</i> operations, invoke
   * this method to prepare for a sequence of channel-write or relative
   * <i>get</i> operations.  For example:
   *
   * <blockquote><pre>
   * buf.put(magic);    // Prepend header
   * in.read(buf);      // Read data into rest of buffer
   * buf.flip();        // Flip buffer
   * out.write(buf);    // Write header + data to channel</pre></blockquote>
   *
   * <p> This method is often used in conjunction with the {@link
   * java.nio.DataBuffer#compact compact} method when transferring data from
   * one place to another.  </p>
   *
   * @return  This buffer
   */
  AVBlockBuffer& flip() {
    mLimit = mPosition;
    mPosition = 0;
    mMark = -1;
    return *this;
  }

  /**
   * Sets this buffer's mark at its position.
   *
   * @return  This buffer
   */
  AVBlockBuffer& mark() {
    mMark = static_cast<int32_t>(mPosition);
    return *this;
  }

  /**
   * Resets this buffer's position to the previously-marked position.
   *
   * <p> Invoking this method neither changes nor discards the mark's value. </p>
   *
   * @return  This buffer
   */
  AVBlockBuffer& reset() {
    if (mMark >= 0) {
      mPosition = mMark;
    }
    return *this;
  }

  /**
   * Rewinds this buffer.  The position is set to zero and the mark is
   * discarded.
   *
   * <p> Invoke this method before a sequence of channel-write or <i>get</i>
   * operations, assuming that the limit has already been set
   * appropriately.  For example:
   *
   * <blockquote><pre>
   * out.write(buf);    // Write remaining data
   * buf.rewind();      // Rewind buffer
   * buf.get(array);    // Copy data into array</pre></blockquote>
   *
   * @return  This buffer
   */
  AVBlockBuffer& rewind() {
    mMark = -1;
    mPosition = 0;
    return *this;
  }

  /**
   * Compacts this buffer&nbsp;&nbsp;<i>(optional operation)</i>.
   *
   * <p> The bytes between the buffer's current position and its limit,
   * if any, are copied to the beginning of the buffer.  That is, the
   * byte at index <i>p</i>&nbsp;=&nbsp;{@code position()} is copied
   * to index zero, the byte at index <i>p</i>&nbsp;+&nbsp;1 is copied
   * to index one, and so forth until the byte at index
   * {@code limit()}&nbsp;-&nbsp;1 is copied to index
   * <i>n</i>&nbsp;=&nbsp;{@code limit()}&nbsp;-&nbsp;{@code 1}&nbsp;-&nbsp;<i>p</i>.
   * The buffer's position is then set to <i>n+1</i> and its limit is set to
   * its capacity.  The mark, if defined, is discarded.
   *
   * <p> The buffer's position is set to the number of bytes copied,
   * rather than to zero, so that an invocation of this method can be
   * followed immediately by an invocation of another relative <i>put</i>
   * method. </p>
   *
   *
   * <p> Invoke this method after writing data from a buffer in case the
   * write was incomplete.  The following loop, for example, copies bytes
   * from one channel to another via the buffer {@code buf}:
   *
   * <blockquote><pre>{@code
   *   buf.clear();          // Prepare buffer for use
   *   while (in.read(buf) >= 0 || buf.position != 0) {
   *       buf.flip();
   *       out.write(buf);
   *       buf.compact();    // In case of partial write
   *   }
   * }</pre></blockquote>
   *
   * @return  This buffer
   */
  AVBlockBuffer& compact() {
    do {
      if (mPosition >= mLimit) {
        mPosition = 0;
        break;
      }
      for (size_t i = 0; i < mLimit - mPosition; ++i) {
        mBuffer[i] = mBuffer[mPosition + i];
      }
      mPosition = mLimit - mPosition;
    } while (false);

    mLimit = mCapacity;
    return *this;
  }

  AVBlockBuffer& put(const std::shared_ptr<AVBlockBuffer>& src) {
    return put(src.get());
  }

  AVBlockBuffer& put(AVBlockBuffer* src) {
    if (this == src || src == nullptr) {
      return *this;
    }
    const int srcPos = src->position();
    const int srcLim = src->limit();
    const int srcRem = (srcPos <= srcLim ? srcLim - srcPos : 0);
    const int pos = position();
    const int lim = limit();
    int rem = (pos <= lim ? lim - pos : 0);

    putBuffer(pos, src, srcPos, srcRem);

    position(pos + srcRem);
    src->position(srcPos + srcRem);

    return *this;
  }

  /**
   * Absolute bulk put method (optional operation).
   *
   * This method transfers length bytes into this buffer from the given source buffer, starting
   * at the given offset in the source buffer and the given index in this buffer. The positions
   * of both buffers are unchanged.
   *
   * @param index
   *        The index in this buffer at which the first byte will be written;
   *        must be non-negative and less than limit()
   * @param src
   *        The buffer from which bytes are to be read
   * @param offset
   *        The index within the source buffer of the first byte to be read;
   *        must be non-negative and less than src.limit()
   *
   * @param length
   *        The number of bytes to be read from the given buffer;
   *        must be non-negative and no larger than the smaller of limit() - index and
   *        src.limit() - offset
   * @return
   */
  AVBlockBuffer& put(const int index, AVBlockBuffer* src, const int offset, const size_t length) {
    if (src) {
      putBuffer(index, src, offset, length);
    }
    return *this;
  }

  /**
   * Relative put method for writing a uint8_t value (optional operation).
   *
   * Writes one bytes containing the given uint8_t value, in the current byte order, into this buffer
   * at the current position, and then increments the position by one.
   *
   * @param value The uint8_t value to be written
   *
   * @return This buffer
   */
  AVBlockBuffer& put(const uint8_t value) {
    append<uint8_t>(value);
    return *this;
  }

  /**
   * Absolute put method (optional operation).
   *
   * @param index The index at which the byte will be written
   * @param value The byte value to be written
   * @return This buffer
   */
  AVBlockBuffer& put(const size_t index, uint8_t value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative bulk put method (optional operation).
   *
   * @param src The array from which bytes are to be read
   *
   * @param length
   *        The number of bytes to be read from the given array;
   *        must be non-negative and no larger than {@code src.length}
   *
   * @return This buffer
   */
  AVBlockBuffer& put(const uint8_t src[], const size_t length) {
    return put(src, 0, length);
  }

  /**
   * Relative bulk put method (optional operation).
   *
   * @param src
   *        The array from which bytes are to be read
   *
   * @param offset
   *        The offset within the array of the first byte to be read;
   *        must be non-negative and no larger than {@code src.length}
   *
   * @param length
   *        The number of bytes to be read from the given array;
   *        must be non-negative and no larger than {@code src.length - offset}
   *
   * @return This buffer
   */
  AVBlockBuffer& put(const uint8_t src[], const int offset, const size_t length) {
    checkSize(0, length);
    const auto pos = position();
    putArray(pos, src, offset, length);
    position(pos + length);
    return *this;
  }

  /**
   * Relative put method for writing a char value (optional operation).
   *
   * Writes one bytes containing the given char value, in the current byte order, into this buffer
   * at the current position, and then increments the position by one.
   *
   * @param value The char value to be written
   * @return This buffer
   */
  AVBlockBuffer& putChar(const char value) {
    append<char>(value);
    return *this;
  }

  /**
   * Absolute put method for writing a char value (optional operation).
   *
   * Writes one bytes containing the given char value, in the current byte order, into this buffer
   * at the given index.
   *
   * @param index The index at which the bytes will be written
   * @param value The char value to be written
   * @return This buffer
   */
  AVBlockBuffer& putChar(const size_t index, char value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative put method for writing a short value (optional operation).
   *
   * Writes two bytes containing the given short value, in the current byte order, into this buffer
   * at the current position, and then increments the position by two.
   *
   * @param value The short value to be written
   * @return This buffer
   */
  AVBlockBuffer& putShort(uint16_t value) {
    append<uint16_t>(value);
    return *this;
  }

  /**
   * Absolute put method for writing a short value (optional operation).
   *
   * Writes two bytes containing the given short value, in the current byte order, into this buffer
   * at the given index.
   *
   * @param index The index at which the bytes will be written
   * @param value The short value to be written
   * @return This buffer
   */
  AVBlockBuffer& putShort(const size_t index, uint16_t value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative put method for writing an int value (optional operation).
   *
   * Writes four bytes containing the given int value, in the current byte order, into this buffer
   * at the current position, and then increments the position by four.
   *
   * @param value The int value to be written
   * @return This buffer
   */
  AVBlockBuffer& putInt(const uint32_t value) {
    append<uint32_t>(value);
    return *this;
  }

  /**
   * Absolute put method for writing an int value (optional operation).
   *
   * Writes four bytes containing the given int value, in the current byte order, into this buffer
   * at the given index.
   *
   * @param index The index at which the bytes will be written
   * @param value The int value to be written
   * @return This buffer
   */
  AVBlockBuffer& putInt(const size_t index, uint32_t value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative put method for writing a long value (optional operation).
   *
   * Writes eight bytes containing the given long value, in the current byte order, into this
   * buffer at the current position, and then increments the position by eight.
   *
   * @param value The long value to be written
   * @return This buffer
   */
  AVBlockBuffer& putLong(const uint64_t value) {
    append<uint64_t>(value);
    return *this;
  }

  /**
   * Writes eight bytes containing the given long value, in the current byte order, into this
   * buffer at the given index.
   *
   * @param index The index at which the bytes will be written
   * @param value The long value to be written
   * @return This buffer
   */
  AVBlockBuffer& putLong(const size_t index, uint64_t value) {
    const auto size = sizeof(value);
    const auto newPos = index + size;
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    if (mPosition < newPos) {
      position(newPos);
    }
    return *this;
  }

  /**
   * Relative put method for writing a float value (optional operation).
   *
   * Writes four bytes containing the given float value, in the current byte order, into this
   * buffer at the current position, and then increments the position by four.
   *
   * @param value The float value to be written
   * @return This buffer
   */
  AVBlockBuffer& putFloat(const float value) {
    append<float>(value);
    return *this;
  }

  /**
   * Absolute put method for writing a float value (optional operation).
   *
   * Writes four bytes containing the given float value, in the current byte order, into this
   * buffer at the given index.
   *
   * @param index The float value to be written
   * @param value The index at which the bytes will be written
   * @return This buffer
   */
  AVBlockBuffer& putFloat(const size_t index, float value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative put method for writing a double value (optional operation).
   *
   * Writes eight bytes containing the given double value, in the current byte order, into this
   * buffer at the current position, and then increments the position by eight.
   *
   * @param value The double value to be written
   * @return This buffer
   */
  AVBlockBuffer& putDouble(const double value) {
    append<double>(value);
    return *this;
  }

  /**
   * Absolute put method for writing a double value (optional operation).
   *
   * Writes eight bytes containing the given double value, in the current byte order, into this
   * buffer at the given index.
   *
   * @param index The index at which the bytes will be written
   * @param value The double value to be written
   * @return This buffer
   */
  AVBlockBuffer& putDouble(const size_t index, double value) {
    const auto size = sizeof(value);
    checkSize(index, size);
    memcpy(&mBuffer[index], (uint8_t*) &value, size);
    return *this;
  }

  /**
   * Relative get method.
   *
   * Reads the byte at this buffer's current position, and then increments the position.
   *
   * @return The byte at the buffer's current position
   */
  uint8_t get() {
    return read<uint8_t>();
  }

  /**
   * Absolute get method.
   *
   * Reads the byte at the given index.
   *
   * @param index The index from which the byte will be read
   *
   * @return The byte at the given index
   */
  [[nodiscard]] uint8_t get(const size_t index) const {
    return read<uint8_t>(index);
  }

  /**
   * Relative bulk get method.
   * This method transfers bytes from this buffer into the given destination array. An invocation
   * of this method of the form src.get(a) behaves in exactly the same way as the invocation
   * src.get(a, 0, a.length)
   *
   * @param dst The destination array
   * @param length The length of the destination array
   */
  AVBlockBuffer& get(uint8_t dst[], const size_t length) {
    return get(dst, 0, length);
  }

  /**
   * Relative bulk get method
   *
   * This method transfers bytes from this buffer into the given destination array.
   *
   * @param dst
   *        The array into which bytes are to be written
   *
   * @param offset
   *        The offset within the array of the first byte to be written;
   *        must be non-negative and no larger than {@code dst.length}
   *
   * @param length
   *        The maximum number of bytes to be written to the given array;
   *        must be non-negative and no larger than {@code dst.length - offset}
   */
  AVBlockBuffer& get(uint8_t dst[], const size_t offset, const size_t length) {
    if (offset + (sizeof(uint8_t) * length) > mLimit) {
      return *this;
    }
    const auto pos = position();
    const auto src = mBuffer.data() + pos;
    std::memcpy(dst + offset, src, sizeof(uint8_t) * length);
    position(pos + length);
    return *this;
  }

  /**
   * Relative get method for reading a char value.
   *
   * Reads the next one byte at this buffer's current position, composing them into a char value
   * according to the current byte order, and then increments the position by one.
   *
   * @return The char value at the buffer's current position
   */
  char getChar() {
    return read<char>();
  }

  /**
   * Absolute get method for reading a char value.
   *
   * Reads one byte at the given index, composing them into a char value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   *
   * @return The char value at the given index
   */
  [[nodiscard]] char getChar(const size_t index) const {
    return read<char>(index);
  }

  /**
   * Relative get method for reading a short value.
   *
   * Reads the next two bytes at this buffer's current position, composing them into a short value
   * according to the current byte order, and then increments the position by two.
   *
   * @return The short value at the buffer's current position
   */
  uint16_t getShort() {
    return read<uint16_t>();
  }

  /**
   * Absolute get method for reading a short value.
   * Reads two bytes at the given index, composing them into a short value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   * @return The short value at the given index
   */
  [[nodiscard]] uint16_t getShort(const size_t index) const {
    return read<uint16_t>(index);
  }

  /**
   * Relative get method for reading an int value.
   *
   * Reads the next four bytes at this buffer's current position, composing them into an int value
   * according to the current byte order, and then increments the position by four.
   *
   * @return The int value at the buffer's current position
   */
  uint32_t getInt() {
    return read<uint32_t>();
  }

  /**
   * Absolute get method for reading an int value.
   *
   * Reads four bytes at the given index, composing them into a int value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   *
   * @return The int value at the given index
   */
  [[nodiscard]] uint32_t getInt(const size_t index) const {
    return read<uint32_t>(index);
  }

  /**
   * Relative get method for reading a long value.
   *
   * Reads the next eight bytes at this buffer's current position, composing them into a long value
   * according to the current byte order, and then increments the position by eight.
   *
   * @return The long value at the buffer's current position
   */
  uint64_t getLong() {
    return read<uint64_t>();
  }

  /**
   * Absolute get method for reading a long value.
   *
   * Reads eight bytes at the given index, composing them into a long value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   *
   * @return The long value at the given index
   */
  [[nodiscard]] uint64_t getLong(const size_t index) const {
    return read<uint64_t>(index);
  }

  /**
   * Relative get method for reading a float value.
   *
   * Reads the next four bytes at this buffer's current position, composing them into a float value
   * according to the current byte order, and then increments the position by four.
   *
   * @return The float value at the buffer's current position
   */
  float getFloat() {
    return read<float>();
  }

  /**
   * Absolute get method for reading a float value.
   *
   * Reads four bytes at the given index, composing them into a float value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   *
   * @return The float value at the given index
   */
  [[nodiscard]] float getFloat(const size_t index) const {
    return read<float>(index);
  }

  /**
   * Relative get method for reading a double value.
   *
   * Reads the next eight bytes at this buffer's current position, composing them into a double
   * value according to the current byte order, and then increments the position by eight.
   *
   * @return The double value at the buffer's current position
   */
  double getDouble() {
    return read<double>();
  }

  /**
   * Absolute get method for reading a double value.
   *
   * Reads eight bytes at the given index, composing them into a double value according to the
   * current byte order.
   *
   * @param index The index from which the bytes will be read
   *
   * @return The double value at the given index
   */
  [[nodiscard]] double getDouble(const size_t index) const {
    return read<double>(index);
  }

  /**
   * Creates a new buffer that shares this buffer's content.
   *
   * <p> The content of the new buffer will be that of this buffer.  Changes
   * to this buffer's content will be visible in the new buffer, and vice
   * versa; the two buffers' position, limit, and mark values will be
   * independent.
   *
   * <p> The new buffer's capacity, limit, position and mark values will be
   * identical to those of this buffer. The new buffer will be direct if, and
   * only if, this buffer is direct, and it will be read-only if, and only if,
   * this buffer is read-only.  </p>
   *
   * @return  The new buffer
   */
  std::shared_ptr<AVBlockBuffer> duplicate() {
    auto newBuffer = std::make_shared<AVBlockBuffer>(mCapacity);

    newBuffer->put(this);
    newBuffer->limit(mLimit);
    newBuffer->position(mPosition);

    return newBuffer;
  }

  /**
   * Tells whether there are any elements between the current position and
   * the limit.
   *
   * @return  {@code true} if, and only if, there is at least one element
   *          remaining in this buffer
   */
  [[nodiscard]] bool hasRemaining() const {
    return mLimit > mPosition;
  }

  /**
   * Returns the number of elements between the current position and the limit.
   *
   * @return  The number of elements remaining in this buffer
   */
  [[nodiscard]] size_t remaining() const {
    return mPosition < mLimit ? mLimit - mPosition : 0;
  }

  void printInfo() const {
    std::cout << "DataBuffer:\n"
      << "\tmark(" << mMark << "), "
      << "position(" << mPosition << "), "
      << "limit(" << mLimit << "), "
      << "capacity(" << mCapacity << ")."
      << std::endl;
  }

private:
  AVBlockBuffer& putArray(const int index, const uint8_t src[], const int srcOffset, const int length) {
    checkSize(index, length);
    memcpy(&mBuffer[index], src + srcOffset, length);
    return *this;
  }

  void putBuffer(const int pos, AVBlockBuffer* src, const int srcPos, const int n) {
    std::memcpy(&mBuffer[pos], src->array() + srcPos, n);
  }

  void checkSize(const size_t index, const size_t increase) {
    if (index + increase <= mCapacity) {
      return;
    }
    const auto newSize =
      mCapacity + (increase + BUFFER_SIZE_INCREASE - 1) / BUFFER_SIZE_INCREASE * BUFFER_SIZE_INCREASE;
    mBuffer.resize(newSize);
    mCapacity = newSize;
  }

  void checkSize(const size_t increase) {
    checkSize(mPosition, increase);
  }

  template<typename T>
  void append(T data) {
    const auto size = sizeof(data);
    checkSize(size);
    memcpy(&mBuffer[mPosition], (uint8_t*) &data, size);
    position(mPosition + size);
  }

  template<typename T>
  [[nodiscard]] T read(const size_t index) const {
    if (index + sizeof(T) > mLimit) {
      return 0;
    }
    return *((T*) &mBuffer[index]);
  }

  template<typename T>
  T read() {
    T data = read<T>(mPosition);
    position(mPosition + sizeof(T));
    return data;
  }
};

} // namespace coral

#endif //CORAL_MEDIA_BLOCK_BUFFER_H
