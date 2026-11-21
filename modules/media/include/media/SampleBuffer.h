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
#ifndef CORAL_MEDIA_SAMPLE_BUFFER_H
#define CORAL_MEDIA_SAMPLE_BUFFER_H

#include <media/BlockBuffer.h>
#include <media/Buffer.h>
#include <media/Configuration.h>

namespace coral {

/**
 * The audio sample buffer.
 */
class AVSampleBuffer final : public AVFrameBuffer,
                             public std::enable_shared_from_this<AVSampleBuffer> {
public:
  explicit AVSampleBuffer(const AudioConfiguration &config) noexcept
      : AVSampleBuffer(nullptr, config) {
  }

  explicit AVSampleBuffer(Owner<AVSampleBuffer> *owner, const AudioConfiguration &config) noexcept
      : owner(owner),
        configuration(config) {
  }

  ~AVSampleBuffer() override;

  [[nodiscard]] AudioConfiguration getConfiguration() const {
    return configuration;
  }

  void setConfiguration(const AudioConfiguration config) {
    if (configuration != config) {
      configuration = config;
    }
  }

  /**
   * Returns the data buffer of the AVSampleBuffer
   */
  const std::shared_ptr<AVBlockBuffer> &data() const {
    return buffer;
  }

  /**
   * Returns the array of the buffer
   */
  const uint8_t *array() const {
    return buffer != nullptr ? buffer->array() : nullptr;
  }

  /**
   * Returns the size of the buffer
   */
  [[nodiscard]] size_t size() const {
    return buffer ? buffer->limit() : 0;
  }

  /**
   * Initializes the buffer.
   *
   * @param timeUs The presentation timestamp for the buffer, in microseconds.
   * @param size An upper bound on the size of the data that will be written to the buffer.
   *
   * @return The buffer pointer, for convenience.
   */
  const std::shared_ptr<AVBlockBuffer> &init(int64_t timeUs, uint32_t size);

  void clear() override;

  /**
   * Ensures that buffer is large enough to accommodate a writer of a
   * given length at its current position.
   *
   * @param length The length of the writer that must be accommodated, in bytes.
   */
  void ensureSpaceForWrite(size_t length);

  void release() override;

  std::shared_ptr<AVSampleBuffer> asSampleBuffer() override {
    return shared_from_this();
  }

private:
  Owner<AVSampleBuffer> *owner;
  AudioConfiguration configuration;
  std::shared_ptr<AVBlockBuffer> buffer;
};

} // namespace coral

#endif //CORAL_MEDIA_SAMPLE_BUFFER_H
