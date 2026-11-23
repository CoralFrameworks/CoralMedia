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

#ifndef CORAL_CODEC_DARWIN_MEDIA_MUXER_H
#define CORAL_CODEC_DARWIN_MEDIA_MUXER_H

#if defined(__APPLE__)

#include <codec/MediaMuxer.h>

#include <memory>
#include <string>

namespace coral {

/**
 * The implementation of the AVMediaMuxer based on the AVFoundation asset writer.
 *
 * The writer writes the already encoded packets of the tracks into the container, the format
 * description of a track declares its codec, so no additional encoder is inserted: the packets reach
 * the file as they were handed to {@link #writeFrame}.
 *
 * <p>The platform shape of the packets differs from the one of a container: a video packet is
 * converted to the length prefixed form the writer expects, and the timestamps of the packets become
 * the timing information of the samples, so the duration of a packet is derived from the frame rate
 * of a video track and from the number of frames a compressed packet carries for an audio track.
 *
 * <p>The platform objects are held behind a pimpl so that this header stays a plain C++ header: it
 * is included by C++ translation units, which must not see any Objective-C type.
 */
class DarwinMediaMuxer final : public AVMediaMuxer {
public:
  DarwinMediaMuxer();

  ~DarwinMediaMuxer() override;

  bool prepare(std::string url, MuxerFormat format) override;

  int addTrack(const std::shared_ptr<MediaFormat>& format) override;

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override;

  void start() override;

  void stop() override;

  void release() override;

  [[nodiscard]] const char* getName() const override { return "DarwinMediaMuxer"; }

  /**
   * Indicates whether the platform asset writer is able to write the container format.
   */
  static bool isSupportedFormat(MuxerFormat format);

private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_MEDIA_MUXER_H
