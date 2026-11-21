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

#ifndef CORAL_EXTRACTOR_EXTRACTOR_INPUT_H
#define CORAL_EXTRACTOR_EXTRACTOR_INPUT_H

#include <extractor/DataBuffer.h>

#include <utils/ByteBuffer.h>

#include <cstdint>
#include <string>

namespace coral {

/**
 * The value returned by the read and peek methods of {@link AVExtractorInput} when the end of the
 * input was reached. Mirrors ExoPlayer's {@code RESULT_END_OF_INPUT}.
 */
static constexpr long kEndOfInput = -1;

/**
 * Holds the current position of an {@link AVExtractorInput}.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code PositionHolder}. It is the only
 * object that is updated by the input, therefore an extractor never needs to guess how many bytes
 * it consumed.
 *
 * <p>A position holder is not thread safe on its own; it is protected by the reader that owns it.
 */
class AVPositionHolder final {
public:
  AVPositionHolder() noexcept : position_(0) {}

  explicit AVPositionHolder(const long position) noexcept : position_(position) {}

  AVPositionHolder(const AVPositionHolder& other) = delete;

  AVPositionHolder& operator=(const AVPositionHolder& other) = delete;

  ~AVPositionHolder() = default;

  [[nodiscard]] long getPosition() const { return position_; }

  void setPosition(const long position) { position_ = position; }

private:
  long position_;
};

/**
 * Provides read access to the media, and tracks the current read position.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code ExtractorInput}. All the
 * methods that advance the position return the number of bytes that were consumed, or one of the
 * {@code kResult*} constants. A returned value smaller than the requested amount means that the end
 * of the input was reached, which lets an extractor process a truncated file without special
 * casing.
 *
 * <p>Implementations are required to keep a read ahead buffer internally, so that
 * {@link peek} and {@link read} may be freely mixed.
 */
class AVExtractorInput {
public:
  virtual ~AVExtractorInput() = default;

  // ================================================================================================
  // Peek methods
  // ================================================================================================

  /**
   * Peeks at data from the input without advancing the read position.
   *
   * @param target The array to copy the peeked data to.
   * @param offset The offset in {@code target} at which to start copying.
   * @param length The maximum number of bytes to copy.
   *
   * @return The number of bytes copied, or {@code kEndOfInput} when there is no data left.
   */
  virtual long peek(uint8_t target[], size_t offset, size_t length) = 0;

  /**
   * Peeks a single byte.
   *
   * @return The byte, or {@code kEndOfInput} when there is no data left.
   */
  virtual int peekByte() = 0;

  /**
   * Advances the peek position by {@code length} bytes, without changing the read position.
   *
   * @param length The number of bytes to skip.
   */
  virtual void skipPeeked(long length) = 0;

  /**
   * Sets the peek position, relative to the read position.
   *
   * <p>This is the counterpart of an absolute assignment to
   * {@code ExtractorInput.getPeekPosition()} in the Java implementation, and it is what an extractor
   * uses to remember the position of a frame it is about to read.
   *
   * @param position The new peek position, in bytes from the read position.
   */
  virtual void setPeekPosition(long position) = 0;

  /**
   * Resets the peek position to the read position.
   */
  virtual void resetPeekPosition() = 0;

  /**
   * Returns the current peek position, in bytes from the start of the input.
   */
  [[nodiscard]] virtual long getPeekPosition() const = 0;

  /**
   * Returns the current peek position, in bytes from the read position.
   *
   * <p>This is the value that the Java implementation assigns directly to
   * {@code ExtractorInput.getPeekPosition()}, and it is what an extractor uses to remember the
   * position of a frame it is about to read.
   */
  [[nodiscard]] virtual long getPeekPositionOffset() const = 0;

  // ================================================================================================
  // Read methods
  // ================================================================================================

  /**
   * Reads data from the input and advances the read position.
   *
   * @param target The array to copy the read data to.
   * @param offset The offset in {@code target} at which to start copying.
   * @param length The maximum number of bytes to copy.
   *
   * @return The number of bytes copied, or {@code kEndOfInput} when there is no data left.
   */
  virtual long read(uint8_t target[], size_t offset, size_t length) = 0;

  /**
   * Reads data from the input into a byte buffer, and advances the read position.
   *
   * <p>The buffer is grown as needed, cleared and then filled with the read data. On return its
   * position is zero and its limit is the number of bytes that were read.
   *
   * @param target The buffer to read into.
   * @param length The maximum number of bytes to read.
   *
   * @return The number of bytes read, or {@code kEndOfInput} when there is no data left.
   */
  virtual long read(AVDataBuffer& target, size_t length) = 0;

  /**
   * Reads and discards {@code length} bytes.
   *
   * @param length The number of bytes to skip.
   *
   * @return The number of bytes skipped, or {@code kEndOfInput} when the end was reached first.
   */
  virtual long skip(long length) = 0;

  /**
   * Reads and discards all the remaining data of the input.
   */
  virtual void skipFully() = 0;

  /**
   * Returns the current read position, in bytes from the start of the input.
   */
  [[nodiscard]] virtual long getPosition() const = 0;

  /**
   * Returns the length of the input in bytes, or {@code -1} when unknown.
   */
  [[nodiscard]] virtual long getLength() const = 0;

  /**
   * Returns the position holder that is updated with the read position.
   */
  [[nodiscard]] virtual AVPositionHolder* getPositionHolder() = 0;

  /**
   * Sets the read position of the input.
   *
   * <p>Implementations that are not seekable must not report success when the requested position is
   * neither the current one nor a position they can reach by skipping.
   *
   * @param position The new read position, in bytes from the start of the input.
   * @param allowEndOfInput Whether the end of the input is an acceptable outcome.
   *
   * @return {@code true} on success.
   */
  virtual bool setPosition(long position, bool allowEndOfInput) = 0;

  /**
   * Sets the read position of the input, and resets the peek position.
   *
   * @param position The new read position, in bytes from the start of the input.
   * @param allowEndOfInput Whether the end of the input is an acceptable outcome.
   *
   * @return {@code true} on success.
   */
  virtual bool resetPosition(long position, bool allowEndOfInput) = 0;

  // ================================================================================================
  // Fully reading methods
  // ================================================================================================

  /**
   * Reads data, and fails when the end of the input is reached before {@code length} bytes were
   * read.
   *
   * <p>These methods mirror ExoPlayer's {@code readFully} family. They make the extractor code much
   * more readable for the parts of a container that are mandatory, while the partial variants above
   * remain available for the parts that may be truncated.
   *
   * @param target The array to copy the read data to.
   * @param offset The offset in {@code target} at which to start copying.
   * @param length The number of bytes to read.
   *
   * @return {@code true} when exactly {@code length} bytes were read.
   */
  virtual bool readFully(uint8_t target[], size_t offset, size_t length) = 0;

  /**
   * Reads data into a byte buffer, and fails when the end of the input is reached before
   * {@code length} bytes were read.
   *
   * @param target The buffer to read into.
   * @param length The number of bytes to read.
   *
   * @return {@code true} when exactly {@code length} bytes were read.
   */
  virtual bool readFully(AVDataBuffer& target, size_t length) = 0;

  /**
   * Reads and discards {@code length} bytes, and fails when the end of the input is reached first.
   *
   * @param length The number of bytes to skip.
   *
   * @return {@code true} when exactly {@code length} bytes were skipped.
   */
  virtual bool skipFully(long length) = 0;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_EXTRACTOR_INPUT_H
