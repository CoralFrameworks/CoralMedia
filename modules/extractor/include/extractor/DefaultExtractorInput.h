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

#ifndef CORAL_EXTRACTOR_DEFAULT_EXTRACTOR_INPUT_H
#define CORAL_EXTRACTOR_DEFAULT_EXTRACTOR_INPUT_H

#include <datasource/DataSource.h>
#include <extractor/DataBuffer.h>
#include <extractor/ExtractorInput.h>

#include <memory>

namespace coral {

/**
 * The default {@link AVExtractorInput}, backed by a {@link AVDataSource}.
 *
 * <p>Because a data source only provides positional reads, this input keeps a read ahead buffer of
 * {@link #kDefaultBufferSize} bytes that serves both the peek and the read methods. The buffer is
 * refilled on demand, therefore an extractor never needs to know how the underlying media is
 * accessed.
 *
 * <p>The input owns the position holder, so the reader can always observe how far the extractor
 * went. The class is not thread safe on its own: the reader serializes the access to it.
 */
class AVDefaultExtractorInput final : public AVExtractorInput {
public:
  /** The size of the read ahead buffer, in bytes. */
  static constexpr size_t kDefaultBufferSize = 64 * 1024;

  /**
   * @param dataSource The data source to read from. Must not be null, and must already be open.
   * @param position The initial read position.
   */
  explicit AVDefaultExtractorInput(std::shared_ptr<AVDataSource> dataSource, long position = 0);

  ~AVDefaultExtractorInput() override = default;

  AVDefaultExtractorInput(const AVDefaultExtractorInput& other) = delete;

  AVDefaultExtractorInput& operator=(const AVDefaultExtractorInput& other) = delete;

  // ================================================================================================
  // Peek methods
  // ================================================================================================

  long peek(uint8_t target[], size_t offset, size_t length) override;

  int peekByte() override;

  void skipPeeked(long length) override;

  void setPeekPosition(long position) override;

  void resetPeekPosition() override;

  [[nodiscard]] long getPeekPosition() const override { return peekPosition_; }

  [[nodiscard]] long getPeekPositionOffset() const override { return peekPosition_ - position_; }

  /**
   * Peeks data, failing when the end of the input is reached before {@code length} bytes were
   * peeked.
   *
   * @return {@code true} when exactly {@code length} bytes were peeked.
   */
  bool peekFully(uint8_t target[], size_t offset, size_t length);

  // ================================================================================================
  // Read methods
  // ================================================================================================

  long read(uint8_t target[], size_t offset, size_t length) override;

  long read(AVDataBuffer& target, size_t length) override;

  long skip(long length) override;

  void skipFully() override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] long getLength() const override { return length_; }

  [[nodiscard]] AVPositionHolder* getPositionHolder() override { return &positionHolder_; }

  bool setPosition(long position, bool allowEndOfInput) override;

  bool resetPosition(long position, bool allowEndOfInput) override;

  // ================================================================================================
  // Fully reading methods
  // ================================================================================================

  bool readFully(uint8_t target[], size_t offset, size_t length) override;

  bool readFully(AVDataBuffer& target, size_t length) override;

  bool skipFully(long length) override;

  /**
   * Reads data into the given buffer, and fails when the end of the input is reached before
   * {@code length} bytes were read.
   */
  bool readFullyImpl(AVDataBuffer& target, size_t length);

  // ================================================================================================
  // Source management, called by the reader
  // ================================================================================================

  /**
   * Opens the given data source. The input does not own the data source, the reader does.
   *
   * @return {@code true} on success.
   */
  bool setDataSource(const std::shared_ptr<AVDataSource>& dataSource);

  /**
   * Returns the data source this input reads from, possibly null.
   */
  [[nodiscard]] const std::shared_ptr<AVDataSource>& getDataSource() const { return dataSource_; }

private:
  /**
   * The location of the peek position with respect to the read position.
   */
  enum class PeekState {
    /** The peek position is the read position, a read consumes the next bytes of the input. */
    kNone,
    /** The peek position is behind the read position, the peeked bytes were already consumed. */
    kBehind,
    /** The peek position is ahead of the read position, a read consumes the peeked bytes first. */
    kAhead,
  };

  /**
   * Loads the read ahead buffer for the given peek position.
   *
   * @param position The position the buffer must start at.
   * @param forceLoad Whether the buffer must be refilled even when it already starts at
   *                  {@code position}, so that the last partial read is turned into a definite end
   *                  of input.
   *
   * @return The number of bytes that were loaded.
   */
  long loadPeekBuffer(long position, bool forceLoad);

  /**
   * Reads up to {@code requested} bytes at the read position, without copying them out. The read
   * ahead buffer holds the bytes that were consumed on return.
   *
   * @return The number of bytes that were skipped, or {@code kEndOfInput} when there is no data
   *         left.
   */
  long advanceStep(long requested);

  /**
   * Advances the read position, the peek position and the position holder by {@code length} bytes.
   */
  void advancePosition(long length);

  /**
   * Invalidates the read ahead buffer.
   */
  void invalidatePeekBuffer();

  std::shared_ptr<AVDataSource> dataSource_;
  AVDataBuffer peekBuffer_;
  /** The absolute position of the first byte of {@link peekBuffer_}. */
  long bufferStartPosition_ = 0;
  /** The number of valid bytes in {@link peekBuffer_}. */
  long bytesLoaded_ = 0;
  /** The absolute read position. */
  long position_ = 0;
  /** The absolute peek position. */
  long peekPosition_ = 0;
  /** The location of the peek position with respect to the read position. */
  PeekState peekState_ = PeekState::kNone;
  long length_ = -1;
  bool endOfInput_ = false;
  AVPositionHolder positionHolder_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_DEFAULT_EXTRACTOR_INPUT_H
