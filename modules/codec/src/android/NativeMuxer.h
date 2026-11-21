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
#ifndef CORAL_MUXER_ANDROID_NATIVE_MUXER_H
#define CORAL_MUXER_ANDROID_NATIVE_MUXER_H

#include <codec/MediaMuxer.h>

#if defined(__ANDROID__)
#include <media/NdkMediaMuxer.h>
#endif

namespace coral {

/**
 * The implementation of the Muxer based AMediaMuxer
 */
class NativeMuxer final : public AVMediaMuxer {
public:
  NativeMuxer();

  ~NativeMuxer() override;

  bool prepare(std::string url, MuxerFormat format) override;

  int addTrack(const std::shared_ptr<MediaFormat>& format) override;

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override;

  void start() override;

  void stop() override;

  void release() override;

  [[nodiscard]] const char* getName() const override {
    return "NdkMediaMuxer";
  }

private:
  static constexpr int kInvalidTrack = -1;

  struct MuxerDeleter {
    void operator()(AMediaMuxer* muxer) const {
      if (muxer != nullptr) {
        AMediaMuxer_delete(muxer);
      }
    }
  };

  int mFd = -1;
  std::unique_ptr<AMediaMuxer, MuxerDeleter> mediaMuxer;
  AMediaCodecBufferInfo bufferInfo;
};

} // namespace coral

#endif //CORAL_MUXER_ANDROID_NATIVE_MUXER_H
