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

#ifndef CORAL_MEDIA_BUFFER_H
#define CORAL_MEDIA_BUFFER_H

#include <media/Definitions.h>
#include <media/Types.h>

#include <memory>

namespace coral {

class AVPacketBuffer;
class AVPixelBuffer;
class AVSampleBuffer;

/**
 * Base class for buffers with flags
 */
class AVBuffer {
public:
  virtual ~AVBuffer() noexcept;

  /**
   * Flags which can apply to a buffer containing a media sample.
   */
  enum Flags : uint32_t {
    /** Default flags for the buffer. */
    kNone = 0,

    /** Indicates that a buffer holds a synchronization sample. */
    kKeyFrame = 1,

    /**
     * This indicated that the buffer marked as such contains codec initialization / codec
     * specific data instead of media data.
     */
    kCodecConfig = 2,

    /** Indicates for empty buffers that signal that the end of the stream was reached. */
    kEndOfStream = 4,

    /** Indicates that a buffer is known to contain the first media sample of the stream. */
    kFirstSample = (1 << 27),

    /** Indicates that a buffer has supplemental data. */
    kHasSupplementalData = (1 << 28),

    /** Indicates that a buffer is known to contain the last media sample of the stream. */
    kLastSample = (1 << 29),

    /** Indicates that a buffer should be decoded but not rendered. */
    kDiscard = (1 << 30),
  };

  /**
   * Clears the buffer
   */
  virtual void clear() {
    flags = static_cast<int32_t>(Flags::kNone);
  }

  /**
   * Returns whether the {@link Flags::kFirstSample} flag is set.
   * */
  [[nodiscard]] bool isFirstSample() const {
    return getFlag(Flags::kFirstSample);
  }

  /**
   * Returns whether the {@link Flags::kKeyFrame} flag is set.
   */
  [[nodiscard]] bool isKeyFrame() const {
    return getFlag(Flags::kKeyFrame);
  }

  /** Returns whether the {@link Flags::kLastSample} flag is set. */
  [[nodiscard]] bool isLastSample() const {
    return getFlag(Flags::kLastSample);
  }

  /**
   * Returns whether the {@link Flags::kCodecConfig} flag is set.
   */
  [[nodiscard]] bool isCodecConfig() const {
    return getFlag(Flags::kCodecConfig);
  }

  /**
   * Returns whether the {@link Flags::kEndOfStream} flag is set.
   *
   * If this is set, all other attributes of the buffer should be ignored.
   */
  [[nodiscard]] bool isEndOfStream() const {
    return getFlag(Flags::kEndOfStream);
  }

  /**
   * Returns whether the {@link Flags::kDiscard} flag is set.
   */
  [[nodiscard]] virtual bool isDiscard() const {
    return getFlag(Flags::kDiscard);
  }

  /**
   * Returns whether the {@link Flags::kHasSupplementalData} flag is set.
   */
  [[nodiscard]] bool hasSupplementalData() const {
    return getFlag(Flags::kHasSupplementalData);
  }

  /**
   * Replaces this buffer's flags with {@code newFlags}.
   *
   * @param newFlags The flags to set, which should be a combination of the {@code Flags} constants.
   */
  void setFlags(const Flags newFlags) {
    this->flags = static_cast<int32_t>(newFlags);
  }

  /**
   * Adds the {@code flag} to this buffer's flag.
   *
   * @param flag The flag to add to this buffer's flag, which should be one of the {@code
   *     Flags#BUFFER_FLAG_*} constants.
   */
  void addFlag(const Flags flag) {
    this->flags |= static_cast<int32_t>(flag);
  }

  /**
   * Returns the media type of the Buffer
   */
  [[nodiscard]] virtual MediaType getMediaType() const {
    return MediaType::kMediaTypeInvalid;
  }

  /**
   * Returns the time(us) of the Buffer. Returns Definitions::kTimeUnset
   * when timeUs is not available.
   */
  [[nodiscard]] virtual int64_t timeUs() const {
    return Definitions::kTimeUnset;
  }

  /**
   * Updates the presentation timestamp for the buffer, in microseconds.
   *
   * @param timeUs the new presentation timestamp, in microseconds.
   */
  virtual void timeUs(int64_t timeUs) = 0;

  virtual std::shared_ptr<AVPacketBuffer> asPacketBuffer() { return nullptr; }
  virtual std::shared_ptr<AVPixelBuffer> asPixelBuffer() { return nullptr; }
  virtual std::shared_ptr<AVSampleBuffer> asSampleBuffer() { return nullptr; }

protected:
  /**
   * Returns whether the specified flag has been set on this buffer.
   *
   * @param other The flag to check.
   * @return Whether the flag is set.
   */
  [[nodiscard]] bool getFlag(const Flags other) const {
    const auto value = static_cast<int32_t>(other);
    return (flags & value) == value;
  }

private:
  int32_t flags = Flags::kNone;
};

using AVBufferFlags = AVBuffer::Flags;

/**
 * The frame buffer decoded.
 */
class AVFrameBuffer : public AVBuffer {
protected:
  /** The presentation timestamp for the buffer, in microseconds. */
  int64_t timeUs_ = 0;

public:
  /** The owner of the FrameBuffer */
  template<typename AVBuffer>
  class Owner {
  public:
    virtual ~Owner() = default;

    virtual void recycleBuffer(std::shared_ptr<AVBuffer> frameBuffer) = 0;
  };

  /**
   * The number of buffers immediately prior to this one that were skipped in the decoder.
   */
  int skippedOutputBufferCount = 0;

  /**
   * Whether this buffer should be skipped, usually because the decoding process generated no data
   * or invalid data.
   */
  bool shouldBeSkipped = false;

  [[nodiscard]] bool isDiscard() const override {
    return shouldBeSkipped || AVBuffer::isDiscard();
  }

  /** Releases the frame buffer for reuse. Must be called when the buffer is no longer needed. */
  virtual void release() = 0;

  /** The presentation timestamp for the buffer, in microseconds. */
  [[nodiscard]] int64_t timeUs() const override {
    return timeUs_;
  }

  /**
   * Updates the presentation timestamp for the buffer, in microseconds.
   *
   * @param timeUs the new presentation timestamp, in microseconds.
   */
  void timeUs(const int64_t timeUs) override {
    this->timeUs_ = timeUs;
  }

  void clear() override;
};

} // namespace coral

#endif //CORAL_MEDIA_BUFFER_H
