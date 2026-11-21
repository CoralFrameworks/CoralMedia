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

#ifndef CORAL_CODEC_ANDROID_MEDIA_MUXER_H
#define CORAL_CODEC_ANDROID_MEDIA_MUXER_H

#include <codec/MediaMuxer.h>

#include <core/jni/android/MediaMuxer.h>

#include <memory>

namespace coral {

/**
 * The muxer implementation based on android.media.MediaMuxer
 */
class MediaMuxer final : public AVMediaMuxer {
public:
  MediaMuxer();

  bool prepare(std::string url, MuxerFormat format) override;

  int addTrack(const std::shared_ptr<MediaFormat>& format) override;

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override;

  void start() override;

  void stop() override;

  void release() override;

  [[nodiscard]] const char* getName() const override { return "MediaMuxer"; }

private:
  static bool supportMuxerFormat(MuxerFormat format);

  static JniMediaMuxer::OutputFormat convertMuxerFormat(MuxerFormat format);

  static constexpr int DEFAULT_MAX_INPUT_SIZE = 2048 * 2048;

  // ByteBuffer allocation size
  int maxInputSize = DEFAULT_MAX_INPUT_SIZE;

  std::unique_ptr<JniMediaMuxer> muxer;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIA_MUXER_H
