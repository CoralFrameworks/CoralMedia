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

#ifndef CORAL_CODEC_MUXER_H
#define CORAL_CODEC_MUXER_H

#include <codec/CodecTypes.h>

#include <media/Format.h>
#include <media/PacketBuffer.h>

#include <atomic>
#include <memory>
#include <string>

namespace coral {

/**
 * The AVMediaMuxer interface to mux audio and video
 */
class AVMediaMuxer {
public:
  virtual ~AVMediaMuxer() = default;

  /**
   * The status of the muxer operation
   */
  enum class Status : uint8_t {
    /**
     * The muxer is idle.
     */
    kMuxerStatusIdle = 0,
    /**
     * The muxer is prepared
     */
    kMuxerStatusPrepared = 1,
    /**
     * The muxer is started
     */
    kMuxerStatusStarted = 2,
    /**
     * The muxer is stopped
     */
    kMuxerStatusStopped = 3,
    /**
     * The muxer is error
     */
    kMuxerStatusError = 4,
  };

  static constexpr int DEFAULT_AUDIO_BIT_RATE = 128000;

  static constexpr int DEFAULT_FRAME_RATE = 30;

  static constexpr int kInvalidTrack = Definitions::kInvalidValue;

  /**
   * Sets the data source
   */
  virtual bool prepare(std::string url, MuxerFormat format) = 0;

  /**
   * Add track to the muxer delegate
   *
   * @param format    The media format
   * @return          Returns the track index if success, otherwise, returns kInvalidTrack.
   */
  virtual int addTrack(const std::shared_ptr<MediaFormat>& format) = 0;

  /**
   * Writes the packet buffer to the muxer delegate
   *
   * @param buffer the packet buffer to write
   */
  virtual void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) = 0;

  /**
   * Starts the muxer
   */
  virtual void start() = 0;

  /**
   * Stops the muxer
   */
  virtual void stop() = 0;

  /**
   * Releases the muxer
   */
  virtual void release() = 0;

  /**
   * Returns the name of the MuxerDelegate
   */
  virtual const char* getName() const = 0;

  /**
   * Indicates whether the muxer is started
   */
  [[nodiscard]] bool isStarted() const {
    return mStatus == Status::kMuxerStatusStarted;
  }

  /**
   * Returns the muxer status.
   */
  [[nodiscard]] Status getStatus() const { return mStatus; }

protected:
  std::atomic<Status> mStatus = Status::kMuxerStatusIdle;
};

/**
 * The factory for creating an AVMediaMuxer.
 */
class MediaMuxerFactory {
public:
  virtual ~MediaMuxerFactory() = default;

  /**
   * Whether muxer implementation supports MuxerFormat
   */
  virtual bool isSupportMuxerFormat(MuxerFormat muxerFormat) = 0;

  /**
   * Create a new muxer instance
   */
  virtual std::unique_ptr<AVMediaMuxer> createMuxer() = 0;
};

/**
 * Creates a new factory that can create the built-in types of muxers.
 */
std::unique_ptr<MediaMuxerFactory> CreateBuiltinMuxerFactory();

} // namespace coral

#endif // CORAL_CODEC_MUXER_H
