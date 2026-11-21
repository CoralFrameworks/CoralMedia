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

/**
 * A demo which prints the tracks and the samples of a media file by using the platform extractor.
 *
 * usage: codec_media_info [media file]
 *
 * Without an argument the demo creates a short m4a file in the temporary directory first.
 */

#include <codec/AudioFileWriter.h>
#include <codec/MediaExtractor.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace coral;

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNEL_COUNT = 2;
constexpr int DURATION_SECONDS = 2;

std::string createDemoFile() {
  const auto path = (std::filesystem::temp_directory_path() / "coral-media-info.m4a").string();
  std::error_code error;
  std::filesystem::remove(path, error);

  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, SAMPLE_RATE, CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  format->setAverageBitrate(128000);

  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC, format);
  if (writer == nullptr) {
    return {};
  }
  Error* outError = nullptr;
  if (!writer->open(&outError)) {
    MINILOG_ERROR("failed to open the demo file: {}",
      outError != nullptr ? outError->toString() : "");
    return {};
  }

  constexpr double PI = 3.14159265358979323846;
  const int frameCount = SAMPLE_RATE * DURATION_SECONDS;
  const int chunkFrames = 1024;
  std::vector<int16_t> chunk(static_cast<size_t>(chunkFrames) * CHANNEL_COUNT);
  int written = 0;
  while (written < frameCount) {
    const auto frames = std::min(chunkFrames, frameCount - written);
    for (int frame = 0; frame < frames; ++frame) {
      const auto value = static_cast<int16_t>(
        std::sin(2.0 * PI * 440.0 * (written + frame) / SAMPLE_RATE) * 12000.0);
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
        chunk[static_cast<size_t>(frame) * CHANNEL_COUNT + channel] = value;
      }
    }
    if (writer->writeFromPCMBuffer(chunk.data(), frames, &outError) != frames) {
      MINILOG_ERROR("failed to write the demo file: {}",
        outError != nullptr ? outError->toString() : "");
      return {};
    }
    written += frames;
  }
  writer->close();
  printf("created the demo file: %s\n", path.c_str());
  return path;
}

class InfoOutput final : public AVMediaExtractor::Output {
public:
  struct TrackInfo {
    MediaType type = MediaType::kMediaTypeInvalid;
    std::shared_ptr<MediaFormat> format;
    int64_t samples = 0;
    int64_t bytes = 0;
  };

  class TrackCollector final : public AVTrackOutput::Consumer {
  public:
    explicit TrackCollector(TrackInfo* info) : mInfo(info) {}

    void setFormat(const std::shared_ptr<MediaFormat>& format) override {
      mInfo->format = format;
    }

    void consume(const AVSample& sample) override {
      mInfo->samples++;
      mInfo->bytes += static_cast<int64_t>(sample.size());
    }

  private:
    TrackInfo* mInfo;
  };

  std::shared_ptr<AVTrackOutput> track(int trackId, MediaType type,
    AVTrackOutput::SampleConsumer) override {
    // The consumer the extractor offers is not used: the platform extractor delivers the samples
    // through the track output, which is bound to the collector of this output.
    auto& info = mTracks[trackId];
    info.type = type;
    auto output = std::make_shared<TrackCollector>(&info);
    mOutputs[trackId] = output;
    return std::make_shared<AVTrackOutput>(trackId, output);
  }

  std::shared_ptr<AVTrackOutput> track(const int trackId, AVTrackOutput::SampleConsumer consumer) override {
    // The media type of the track is not known, the samples are collected like the ones of the
    // tracks whose type was reported.
    return track(trackId, MediaType::kMediaTypeDefault, std::move(consumer));
  }

  void endTracks() override {
  }

  void seekMap(std::shared_ptr<AVSeekMap> map) override {
    mSeekMap = std::move(map);
  }

  void print() const {
    printf("tracks: %zu\n", mTracks.size());
    for (const auto& [id, info]: mTracks) {
      printf("  track %d: %s, samples=%lld, bytes=%lld\n", id,
        info.type == MediaType::kMediaTypeAudio ? "audio" : "video",
        static_cast<long long>(info.samples), static_cast<long long>(info.bytes));
      if (info.format != nullptr) {
        printf("    format: %s\n", info.format->toString().c_str());
      }
    }
    if (mSeekMap != nullptr) {
      printf("  seekable: %s\n", mSeekMap->isSeekable() ? "yes" : "no");
    }
  }

  std::map<int, TrackInfo> mTracks;
  std::map<int, std::shared_ptr<TrackCollector>> mOutputs;
  std::shared_ptr<AVSeekMap> mSeekMap;
};

} // namespace anonymous

int main(int argc, char** argv) {
  std::string url;
  if (argc > 1) {
    url = argv[1];
  } else {
    url = createDemoFile();
    if (url.empty()) {
      printf("failed to create the demo media file\n");
      return 1;
    }
  }

  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(url);
  if (extractor == nullptr) {
    printf("the file is not supported by the platform extractor: %s\n", url.c_str());
    return 1;
  }

  auto output = std::make_shared<InfoOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  printf("file: %s\n", url.c_str());
  printf("duration: %.3f s\n", extractor->getDurationUs() / 1000000.0);
  output->print();
  extractor->clear();
  return 0;
}
