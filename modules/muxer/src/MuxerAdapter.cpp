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

#include <muxer/MuxerAdapter.h>
#include <muxer/MuxerFactory.h>

#include <minilog/log.h>

#include <utility>

namespace coral {
namespace muxer {
namespace {

bool containerForFormat(const MuxerFormat format, Container* container) {
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4:
      *container = Container::kMp4;
      return true;
    case MuxerFormat::kMuxerFormatWebm:
      *container = Container::kWebM;
      return true;
    case MuxerFormat::kMuxerFormatOgg:
    case MuxerFormat::kMuxerFormatOpus:
      *container = Container::kOgg;
      return true;
    default:
      return false;
  }
}

class MuxerAdapter final : public AVMediaMuxer {
public:
  MuxerAdapter() = default;
  ~MuxerAdapter() override {
    MuxerAdapter::release();
  }

  bool prepare(std::string url, const MuxerFormat format) override {
    if (mMuxer != nullptr &&
        (mStatus == Status::kMuxerStatusPrepared || mStatus == Status::kMuxerStatusStarted)) {
      return true;
    }
    Container container = Container::kMp4;
    if (!containerForFormat(format, &container)) {
      MINILOG_ERROR("the native muxer does not support this container format");
      return false;
    }
    auto factory = createMuxerFactory();
    if (factory == nullptr) {
      return false;
    }
    Error error;
    MuxerOptions options;
    mMuxer = factory->create(url, container, options, &error);
    if (mMuxer == nullptr) {
      MINILOG_ERROR("the native muxer could not open the output: {}", error.message);
      mStatus = Status::kMuxerStatusError;
      return false;
    }
    mStatus = Status::kMuxerStatusPrepared;
    return true;
  }

  int addTrack(const std::shared_ptr<MediaFormat>& format) override {
    if (mMuxer == nullptr || mStatus != Status::kMuxerStatusPrepared) {
      return kInvalidTrack;
    }
    TrackOptions options;
    if (format != nullptr) {
      options.language = format->getLanguage();
    }
    const int track = mMuxer->addTrack(format, options);
    if (track < 0) {
      MINILOG_ERROR("the native muxer rejected a track: {}", mMuxer->getError().message);
      mStatus = Status::kMuxerStatusError;
    }
    return track;
  }

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override {
    if (mMuxer == nullptr || mStatus != Status::kMuxerStatusStarted || buffer == nullptr) {
      return;
    }
    if (buffer->isEndOfStream() || buffer->isCodecConfig() || buffer->flagsOnly() ||
        buffer->size() == 0) {
      return;
    }
    const int track = buffer->streamIndex();
    if (track < 0) {
      return;
    }
    if (!mMuxer->writeSample(track, buffer)) {
      MINILOG_ERROR("the native muxer rejected a sample: {}", mMuxer->getError().message);
      mStatus = Status::kMuxerStatusError;
    }
  }

  void start() override {
    if (mMuxer != nullptr && mStatus == Status::kMuxerStatusPrepared) {
      mStatus = Status::kMuxerStatusStarted;
    }
  }

  void stop() override {
    if (mMuxer == nullptr || mStatus != Status::kMuxerStatusStarted) {
      return;
    }
    if (!mMuxer->finish()) {
      MINILOG_ERROR("the native muxer could not finish the file: {}",
        mMuxer->getError().message);
      mStatus = Status::kMuxerStatusError;
      return;
    }
    mStatus = Status::kMuxerStatusStopped;
  }

  void release() override {
    if (mMuxer != nullptr && mStatus != Status::kMuxerStatusStopped) {
      // A prepared but never started file, and a failed write, must not leave a partial output.
      mMuxer->cancel();
    }
    mMuxer.reset();
    if (mStatus != Status::kMuxerStatusError) {
      mStatus = Status::kMuxerStatusIdle;
    }
  }

  [[nodiscard]] const char* getName() const override {
    return "CoralMuxer";
  }

private:
  std::unique_ptr<Muxer> mMuxer;
};

class MuxerAdapterFactory final : public MediaMuxerFactory {
public:
  bool isSupportMuxerFormat(const MuxerFormat format) override {
    Container container = Container::kMp4;
    return containerForFormat(format, &container);
  }

  std::unique_ptr<AVMediaMuxer> createMuxer() override {
    return std::make_unique<MuxerAdapter>();
  }
};

} // namespace

std::unique_ptr<AVMediaMuxer> createMuxerAdapter() {
  return std::make_unique<MuxerAdapter>();
}

std::unique_ptr<MediaMuxerFactory> createMuxerAdapterFactory() {
  return std::make_unique<MuxerAdapterFactory>();
}

} // namespace muxer
} // namespace coral
