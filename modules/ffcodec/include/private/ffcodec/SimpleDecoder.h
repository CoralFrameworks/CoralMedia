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
#ifndef CORAL_CODEC_SIMPLE_DECODER_H
#define CORAL_CODEC_SIMPLE_DECODER_H

#include <codec/Error.h>
#include <media/Buffer.h>
#include <media/Format.h>

#include <minilog/log.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace coral {

/**
 * Base class for {@link SimpleDecoder}s that use their own decode thread and decode each input
 * buffer immediately into a corresponding output buffer.
 */
template<typename I, typename O>
class SimpleDecoder {
private:
  mutable std::condition_variable notify_cv_;
  mutable std::mutex pending_lock_;
  std::unique_ptr<std::thread> thread;

  std::deque<std::shared_ptr<I>> queuedInputBuffers;
  std::deque<std::shared_ptr<O>> queuedOutputBuffers;
  std::vector<std::shared_ptr<I>> availableInputBuffers;
  std::vector<std::shared_ptr<O>> availableOutputBuffers;

  int availableInputBufferCount;
  int availableOutputBufferCount;
  std::shared_ptr<I> dequeuedInputBuffer;

  std::optional<Error> error;
  bool flushed;
  bool released;
  bool requestEof;
  int skippedOutputBufferCount;
  int64_t outputStartTimeUs;

public:
  virtual void configure(const std::shared_ptr<MediaFormat> &format) {
    auto maxInputBufferSize = format->getMaxInputSize();
    for (auto i = 0; i < availableInputBuffers.size(); ++i) {
      availableInputBuffers[i] = std::move(createInputBuffer());
      availableInputBuffers[i]->ensureSpaceForWrite(maxInputBufferSize);
    }
    for (auto i = 0; i < availableOutputBuffers.size(); ++i) {
      availableOutputBuffers[i] = std::move(createOutputBuffer());
    }
  }

  void start() {
    if (!thread) {
      thread = std::make_unique<std::thread>(&SimpleDecoder::run, this);
    }
  }

  void stop() {
    {
      std::unique_lock<std::mutex> lock(pending_lock_);
      released = true;
      lock.unlock();
      notify_cv_.notify_all();
    }
    if (thread) {
      if (thread->joinable()) {
        thread->join();
      } else {
        thread->detach();
      }
    }
    thread.reset();
  }

  void setOutputStartTimeUs(int64_t newStartTimeUs) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    this->outputStartTimeUs = newStartTimeUs;
  }

  std::shared_ptr<I> dequeueInputBuffer() {
    static const std::shared_ptr<I> sEmptyBuffer = nullptr;
    std::unique_lock<std::mutex> lock(pending_lock_);
    maybeNotifyError();
    dequeuedInputBuffer =
        availableInputBufferCount == 0
            ? sEmptyBuffer
            : availableInputBuffers[--availableInputBufferCount];
    return dequeuedInputBuffer;
  }

  void queueInputBuffer(std::shared_ptr<I> inputBuffer) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    maybeNotifyError();
    queuedInputBuffers.push_back(std::move(inputBuffer));
    maybeNotifyProcessLoop();
    dequeuedInputBuffer = nullptr;
  }

  std::shared_ptr<O> dequeueOutputBuffer() {
    static const std::shared_ptr<O> sEmptyBuffer = nullptr;
    std::unique_lock<std::mutex> lock(pending_lock_);
    maybeNotifyError();
    if (queuedOutputBuffers.empty()) {
      return sEmptyBuffer;
    }
    auto outputBuffer = queuedOutputBuffers.front();
    queuedOutputBuffers.pop_front();
    return outputBuffer;
  }

  void signalEndOfInputStream() {
    auto inputBuffer = dequeueInputBuffer();
    if (inputBuffer != nullptr) {
      inputBuffer->clear();
      inputBuffer->setFlags(AVBufferFlags::kEndOfStream);
      queueInputBuffer(inputBuffer);
    } else {
      std::unique_lock<std::mutex> lock(pending_lock_);
      requestEof = true;
      maybeNotifyProcessLoop();
    }
  }

  void flush() {
    std::unique_lock<std::mutex> lock(pending_lock_);
    requestEof = false;
    flushed = true;
    skippedOutputBufferCount = 0;
    if (dequeuedInputBuffer != nullptr) {
      releaseInputBufferInternal(std::move(dequeuedInputBuffer));
      dequeuedInputBuffer = nullptr;
    }
    while (!queuedInputBuffers.empty()) {
      releaseInputBufferInternal(queuedInputBuffers.front());
      queuedInputBuffers.pop_front();
    }
    while (!queuedOutputBuffers.empty()) {
      queuedOutputBuffers.front()->release();
      queuedOutputBuffers.pop_front();
    }
  }

  void release() {
    stop();
    onRelease();
  }

  virtual const char *getName() const {
    return "SimpleDecoder";
  }

protected:
  explicit SimpleDecoder(int availableInputBufferCount, int availableOutputBufferCount) noexcept
      : availableInputBufferCount(availableInputBufferCount),
        availableOutputBufferCount(availableOutputBufferCount),
        flushed(false),
        released(false),
        requestEof(false),
        skippedOutputBufferCount(0),
        outputStartTimeUs(Definitions::kTimeUnset) {
    availableInputBuffers.resize(availableInputBufferCount);
    availableOutputBuffers.resize(availableOutputBufferCount);
  }

  /**
   * Returns whether a sample time is greater or equal to the {@link setOutputStartTimeUs output
   * start time}, if set.
   *
   * <p>If this method returns false, the buffer will not be made available as an output buffer.
   *
   * @param timeUs The buffer time, in microseconds.
   * @return Whether the buffer time is greater or equal to the output start time, or {@code true}
   *     if the output start time is not set.
   */
  bool isAtLeastOutputStartTimeUs(const int64_t timeUs) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    return outputStartTimeUs == Definitions::kTimeUnset || timeUs >= outputStartTimeUs;
  }

  /**
   * Releases an input buffer back to the MediaCodec.
   *
   * @param inputBuffer The input buffer being released.
   */
  void releaseInputBuffer(std::shared_ptr<I> inputBuffer) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    releaseInputBufferInternal(std::move(inputBuffer));
    maybeNotifyProcessLoop();
  }

  /**
   * Releases an output buffer back to the MediaCodec.
   *
   * @param outputBuffer The output buffer being released.
   */
  void releaseOutputBuffer(std::shared_ptr<O> outputBuffer) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    releaseOutputBufferInternal(std::move(outputBuffer));
    maybeNotifyProcessLoop();
  }

  /** Creates a new input buffer. */
  virtual std::shared_ptr<I> createInputBuffer() = 0;

  /** Creates a new output buffer. */
  virtual std::shared_ptr<O> createOutputBuffer() = 0;

  /**
   * Processes the {@code inputBuffer} and stores any codec output in {@code outputBuffer}.
   *
   * @param inputBuffer The buffer to process.
   * @param outputBuffer The output buffer to store processed data. The output buffer will not be
   *     made available to dequeue if its {@link DecodedBuffer#timeUs} is not {@linkplain
   *     #isAtLeastOutputStartTimeUs at least the output start time} or when it's marked with
   *     {@link DecodedBuffer#shouldBeSkipped}. The output buffer may not have been populated in
   *     these cases.
   * @param reset Whether the MediaCodec must be reset before decoding or encoding.
   * @return A MediaCodec error if an error occurred, or std::nullopt if decoding or encoding
   *     was successful.
   */
  virtual std::optional<Error>
  decodeBuffer(const std::shared_ptr<I> &inputBuffer, const std::shared_ptr<O> &outputBuffer, bool reset) = 0;

  virtual void onRelease() {
    // Do nothing
  }

  std::shared_ptr<O> dequeueAvailableOutputBuffer() {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (availableOutputBufferCount > 0) {
      return dequeueAvailableOutputBufferInternal();
    }
    return nullptr;
  }

  void queueOutputBuffer(const std::shared_ptr<O> &outputBuffer) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (outputBuffer) {
      queueOutputBufferInternal(outputBuffer);
    }
  }

private:
  void maybeNotifyError() {
    if (error != std::nullopt) {
      MINILOG_ERROR("error: {}", error->toString());
    }
  }

  /**
   * Notifies the process loop if there exists a queued input buffer and an available output buffer
   * to process into.
   *
   * <p>Should only be called whilst synchronized on the lock object.
   */
  void maybeNotifyProcessLoop() const {
    if (canProcessBuffer()) {
      notify_cv_.notify_all();
    }
  }

  void run() {
    while (process()) {
      // Do nothing.
    }
  }

  bool process() {
    std::shared_ptr<I> inputBuffer;
    std::shared_ptr<O> outputBuffer;
    bool resetMediaCodec;

    {
      std::unique_lock<std::mutex> lock(pending_lock_);
      while (!released && !canProcessBuffer()) {
        notify_cv_.wait(lock);
      }
      if (released) {
        return false;
      }
      inputBuffer = queuedInputBuffers.front();
      queuedInputBuffers.pop_front();
      outputBuffer = dequeueAvailableOutputBufferInternal();
      resetMediaCodec = flushed;
      flushed = false;
    }

    if (inputBuffer->isEndOfStream()) {
      outputBuffer->addFlag(AVBufferFlags::kEndOfStream);
    } else {
      outputBuffer->timeUs(inputBuffer->timeUs());
      if (inputBuffer->isFirstSample()) {
        outputBuffer->addFlag(AVBufferFlags::kFirstSample);
      }
      if (!isAtLeastOutputStartTimeUs(inputBuffer->timeUs())) {
        outputBuffer->shouldBeSkipped = true;
      }
      auto result = decodeBuffer(inputBuffer, outputBuffer, resetMediaCodec);
      if (result != std::nullopt) {
        std::unique_lock<std::mutex> lock(pending_lock_);
        error = result;
        return false;
      }
    }

    std::unique_lock<std::mutex> lock(pending_lock_);
    if (flushed) {
      outputBuffer->release();
    } else if (outputBuffer->shouldBeSkipped) {
      skippedOutputBufferCount++;
      outputBuffer->release();
    } else {
      outputBuffer->skippedOutputBufferCount = skippedOutputBufferCount;
      skippedOutputBufferCount = 0;
      queueOutputBufferInternal(outputBuffer);
    }
    // Make the input buffer available again.
    releaseInputBufferInternal(inputBuffer);

    // handle end of stream signal
    if (requestEof && availableInputBufferCount != 0) {
      requestEof = false;
      auto buffer = availableInputBuffers[--availableInputBufferCount];
      buffer->clear();
      buffer->setFlags(AVBufferFlags::kEndOfStream);
      queuedInputBuffers.push_back(buffer);
    }

    return true;
  }

  [[nodiscard]] bool canProcessBuffer() const {
    return !queuedInputBuffers.empty() && availableOutputBufferCount > 0;
  }

  std::shared_ptr<O> dequeueAvailableOutputBufferInternal() {
    return availableOutputBuffers[--availableOutputBufferCount];
  }

  void queueOutputBufferInternal(std::shared_ptr<O> outputBuffer) {
    queuedOutputBuffers.push_back(std::move(outputBuffer));
  }

  void releaseInputBufferInternal(std::shared_ptr<I> inputBuffer) {
    inputBuffer->clear();
    availableInputBuffers[availableInputBufferCount++] = std::move(inputBuffer);
  }

  void releaseOutputBufferInternal(std::shared_ptr<O> outputBuffer) {
    outputBuffer->clear();
    availableOutputBuffers[availableOutputBufferCount++] = std::move(outputBuffer);
  }
};

} // namespace coral

#endif //CORAL_CODEC_SIMPLE_DECODER_H
