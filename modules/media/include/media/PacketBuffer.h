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

#ifndef CORAL_MEDIA_PACKET_BUFFER_H
#define CORAL_MEDIA_PACKET_BUFFER_H

#include <media/BlockBuffer.h>
#include <media/Buffer.h>
#include <media/CryptoInfo.h>
#include <media/Format.h>
#include <media/Types.h>

#include <memory>

namespace coral {

/**
 * The Packet Buffer for encoded audio/video data
 */
class AVPacketBuffer : public AVBuffer,
                       public std::enable_shared_from_this<AVPacketBuffer> {
public:
  class Recycler {
  public:
    virtual ~Recycler() = default;

    /**
     * recycle the PacketBuffer when call release().
     *
     * @param packetBuffer the PacketBuffer to recycle.
     */
    virtual void recycleBuffer(std::shared_ptr<AVPacketBuffer> packetBuffer) = 0;
  };

  std::shared_ptr<AVPacketBuffer> asPacketBuffer() override {
    return shared_from_this();
  }
protected:
  Recycler* recycler;

  /**
   * The media type of the PacketBuffer
   */
  MediaType mediaType = MediaType::kMediaTypeInvalid;

  int mStreamIndex = -1;

  /**
   * The {@link MediaFormat}.
   */
  std::shared_ptr<MediaFormat> mFormat;
  bool formatChanged = false;

  /**
   * {@link CryptoInfo} for encrypted data.
   */
  std::shared_ptr<CryptoInfo> mCryptoInfo;

  /**
   * The buffer's data, or {@code null} if no data has been set.
   */
  std::shared_ptr<AVBlockBuffer> mData;

  /**
   * Whether the last attempt to read a sample into this buffer failed due to not yet having the DRM
   * keys associated with the next sample.
   */
  bool waitingForKeys = false;

  /** The time at which the sample should be presented. */
  int64_t presentationTimeUs;

  /**
   * Supplemental data related to the buffer, if {@link hasSupplementalData()} returns true. If
   * present, the buffer is populated with supplemental data from position 0 to its limit.
   */
  std::shared_ptr<AVBlockBuffer> mSupplementalData;

  int paddingSize;

public:
  /**
   * The number of buffers immediately prior to this one that were skipped.
   */
  int skippedOutputBufferCount = 0;

  /**
   * Whether this buffer should be skipped, usually because the decoding process generated no data
   * or invalid data.
   */
  bool shouldBeSkipped = false;

  /**
   * Creates a flags only instance
   */
  static std::shared_ptr<AVPacketBuffer> newFlagsOnlyInstance();

  explicit AVPacketBuffer(const MediaType& mediaType) noexcept
    : AVPacketBuffer(mediaType, 0) {
  }

  explicit AVPacketBuffer(const MediaType& mediaType, int paddingSize) noexcept
    : AVPacketBuffer(nullptr, mediaType, paddingSize) {
  }

  explicit AVPacketBuffer(Recycler* recycler, const MediaType& mediaType) noexcept
    : AVPacketBuffer(recycler, mediaType, 0) {
  }

  explicit AVPacketBuffer(Recycler* recycler, const MediaType& mediaType, int paddingSize) noexcept
    : recycler(recycler),
      mediaType(mediaType),
      presentationTimeUs(0),
      paddingSize(paddingSize) {
  }

  ~AVPacketBuffer() override {
    recycler = nullptr;
  }

  /**
   * Clears the supplemental data and ensures that it's large enough to
   * accommodate {@link length} bytes
   * @param length The length of the supplemental data that must be
   * accommodated, in bytes.
   */
  void resetSupplementalData(int length);

  /**
   * Returns supplemental data related to the buffer.
   */
  [[nodiscard]] const std::shared_ptr<AVBlockBuffer>& supplementalData() const {
    return mSupplementalData;
  }

  /**
   * Ensures that {@link data} is large enough to accommodate a writer of a given length at its current position.
   *
   * @param length    The length of the writer that must be accommodated, in bytes.
   */
  void ensureSpaceForWrite(size_t length);

  /**
   * Returns the data of the packet buffer
   */
  [[nodiscard]] AVBlockBuffer* data() const {
    return mData.get();
  }

  /**
   * Returns the array of the ByteBuffer
   */
  [[nodiscard]] const uint8_t* array() const {
    return mData != nullptr ? mData->array() : nullptr;
  }

  /**
   * Returns the mutable array of the ByteBuffer
   */
  uint8_t* array() {
    return mData != nullptr ? mData->array() : nullptr;
  }

  /**
   * Returns the {@link CryptoInfo} for encrypted data.
   */
  const std::shared_ptr<CryptoInfo>& cryptoInfo() const {
    return mCryptoInfo;
  }

  /**
   * Returns the size of the packet buffer
   */
  [[nodiscard]] size_t size() const {
    return mData != nullptr ? mData->limit() : 0;
  }

  [[nodiscard]] int64_t timeUs() const override {
    return presentationTimeUs;
  }

  void timeUs(const int64_t timeUs) override {
    presentationTimeUs = timeUs;
  }

  /**
   * Sets the new streamIndex to the buffer
   */
  void setStreamIndex(const int streamIndex) {
    mStreamIndex = streamIndex;
  }

  /**
   * Returns the stream index of the packet buffer
   */
  [[nodiscard]] virtual int streamIndex() const {
    return mStreamIndex;
  }

  /**
   * Returns whether the buffer is only able to hold flags, meaning {@link data} is null and its flagsOnly is true
   */
  [[nodiscard]] bool flagsOnly() const { return mediaType == MediaType::kMediaTypeInvalid; }

  /**
   * Checks whether it's a format changed buffer. Only available for encoding buffer
   */
  [[nodiscard]] bool isFormatChanged() const { return formatChanged; }

  /**
   * Sets new pointer of the Format
   */
  void setFormat(const std::shared_ptr<MediaFormat>& newFormat) {
    if (mFormat != newFormat) {
      formatChanged = true;
      mFormat = newFormat;
    }
  }

  /**
   * Returns the format of the PacketBuffer
   */
  [[nodiscard]] const std::shared_ptr<MediaFormat>& getFormat() const {
    return mFormat;
  }

  /**
   * Flips {@link data} and {@link supplementalData} in preparation for being queued to a decoder
   * or encoder.
   */
  void flip() const {
    if (mData != nullptr) {
      mData->flip();
    }
    if (mSupplementalData != nullptr) {
      mSupplementalData->flip();
    }
  }

  void clear() override {
    AVBuffer::clear();
    if (mData != nullptr) {
      mData->clear();
    }
    if (mSupplementalData != nullptr) {
      mSupplementalData->clear();
    }
    mFormat.reset();
    formatChanged = false;
    waitingForKeys = false;
  }

  [[nodiscard]] bool isDiscard() const override {
    return shouldBeSkipped || AVBuffer::isDiscard();
  }

  /** Releases the frame buffer for reuse. Must be called when the buffer is no longer needed. */
  void release() {
    if (recycler) {
      recycler->recycleBuffer(shared_from_this());
    }
  }

  void setWaitingForKeys(const bool newWaitingForKeys) {
    this->waitingForKeys = newWaitingForKeys;
  }

  [[nodiscard]] bool isWaitingForKeys() const { return waitingForKeys; }

  [[nodiscard]] MediaType getMediaType() const override { return mediaType; }
};

} // namespace coral

#endif // CORAL_MEDIA_PACKET_BUFFER_H
