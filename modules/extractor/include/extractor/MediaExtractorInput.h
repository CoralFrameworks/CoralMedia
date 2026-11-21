#ifndef CORAL_EXTRACTOR_MEDIA_EXTRACTOR_INPUT_H
#define CORAL_EXTRACTOR_MEDIA_EXTRACTOR_INPUT_H

#include <extractor/ExtractorInput.h>

#include <cstdint>

namespace coral {

/**
 * The input that a platform extractor reads its media from: the adapter of {@link AVExtractorInput} for
 * the readers of the platforms, e.g. the {@code InputReader} of the media parser of Android, the
 * {@code AVIOContext} of ffmpeg, or the {@code IMFByteStream} of the media foundation.
 *
 * <p>A platform extractor does not open the media itself: the reader of the extractor module owns the
 * input and drives the extractor, and the extractor consumes the media through this class. That is what
 * makes a platform extractor and a built-in one interchangeable, and it is what lets a platform
 * extractor be tried after the built-in ones failed to recognize the container.
 *
 * <p>The adapter holds a reference to the input of the reader, and it never owns it: the input outlives
 * every adapter that is created for it, because the reader keeps it alive while it drives the extractor.
 * It is neither copyable nor movable, so an adapter can neither outlive the reference it holds by being
 * copied somewhere else, nor be left behind in a moved-from state.
 */
class AVMediaExtractorInput final {
public:
  /** The value the reading methods return when there is no data left. */
  static constexpr long kEndOfInput = coral::kEndOfInput;

  explicit AVMediaExtractorInput(AVExtractorInput& input) noexcept
    : input_(input) {
  }

  AVMediaExtractorInput(const AVMediaExtractorInput& other) = delete;

  AVMediaExtractorInput& operator=(const AVMediaExtractorInput& other) = delete;

  AVMediaExtractorInput(AVMediaExtractorInput&& other) = delete;

  AVMediaExtractorInput& operator=(AVMediaExtractorInput&& other) = delete;

  ~AVMediaExtractorInput() = default;

  // ================================================================================================
  // Reading
  // ================================================================================================

  /**
   * Reads data and advances the read position.
   *
   * @return The number of bytes read, or {@link kEndOfInput} when there is no data left.
   */
  long read(uint8_t* target, size_t offset, size_t length) {
    return input_.read(target, offset, length);
  }

  /**
   * Reads data and advances the read position, and fails when the input ends first.
   *
   * @return {@code true} when exactly {@code length} bytes were read.
   */
  bool readFully(uint8_t* target, size_t offset, size_t length) {
    return input_.readFully(target, offset, length);
  }

  /**
   * Reads and discards data.
   *
   * @return The number of bytes skipped, or {@link kEndOfInput} when the end was reached first.
   */
  long skip(long length) { return input_.skip(length); }

  /**
   * Reads and discards {@code length} bytes, and fails when the input ends first.
   */
  bool skipFully(long length) { return input_.skipFully(length); }

  /**
   * Reads and discards the remaining data of the input.
   */
  void skipToEnd() { input_.skipFully(); }

  // ================================================================================================
  // Peeking
  // ================================================================================================

  /**
   * Peeks at data without advancing the read position.
   *
   * @return The number of bytes copied, or {@link kEndOfInput} when there is no data left.
   */
  long peek(uint8_t* target, size_t offset, size_t length) {
    return input_.peek(target, offset, length);
  }

  /**
   * Peeks a single byte, and advances the peek position.
   *
   * @return The byte, or {@link kEndOfInput} when there is no data left.
   */
  int peekByte() { return input_.peekByte(); }

  /**
   * Advances the peek position without changing the read position.
   */
  void skipPeeked(long length) { input_.skipPeeked(length); }

  /**
   * Resets the peek position to the read position.
   */
  void resetPeekPosition() { input_.resetPeekPosition(); }

  /**
   * Returns the peek position, in bytes from the start of the input.
   */
  [[nodiscard]] long getPeekPosition() const { return input_.getPeekPosition(); }

  // ================================================================================================
  // Position
  // ================================================================================================

  /**
   * Returns the read position, in bytes from the start of the input.
   */
  [[nodiscard]] long getPosition() const { return input_.getPosition(); }

  /**
   * Returns the length of the input in bytes, or -1 when it is unknown.
   */
  [[nodiscard]] long getLength() const { return input_.getLength(); }

  /**
   * Returns the position holder of the input, which the reader updates with the bytes the extractor
   * consumed.
   */
  [[nodiscard]] AVPositionHolder* getPositionHolder() { return input_.getPositionHolder(); }

  /**
   * Sets the read position, and resets the peek position.
   *
   * @return {@code true} on success. An input that cannot reach the position reports the failure rather
   *         than reporting a position it did not reach.
   */
  bool setPosition(long position, bool allowEndOfInput) {
    return input_.resetPosition(position, allowEndOfInput);
  }

  /**
   * Returns whether the read position reached the end of the input.
   *
   * <p>The length of an input that is not known, e.g. the response of a server that does not report one,
   * cannot be compared: such an input is at its end once a read returned {@link kEndOfInput}, which the
   * caller sees itself.
   */
  [[nodiscard]] bool isAtEnd() const {
    const long length = input_.getLength();
    return length >= 0 && input_.getPosition() >= length;
  }

  /**
   * Returns the input this adapter forwards to.
   */
  [[nodiscard]] AVExtractorInput& input() const { return input_; }

private:
  AVExtractorInput& input_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_MEDIA_EXTRACTOR_INPUT_H
