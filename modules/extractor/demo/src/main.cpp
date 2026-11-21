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
 * @file main.cpp
 *
 * A command line tool that dumps the tracks and the samples of a media file, using the extractor
 * module of Coral. It doubles as an integration test of the module.
 *
 * Usage:
 *   extractor_demo <media file> [--max-samples <count>] [--time-us <time>] [--file-type <type>]
 *                  [--quiet]
 */

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>

#include <media/Format.h>
#include <media/Types.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using coral::AVDefaultExtractorsFactory;
using coral::AVExtractorsFactory;
using coral::AVFileDataSource;
using coral::AVReader;
using coral::AVSample;
using coral::AVTrackInfo;
using coral::FileTypes;
using coral::MediaFormat;
using coral::MediaType;

/** The callback that consumes the samples of a track. */
using AVSampleConsumer = coral::AVTrackOutput::SampleConsumer;

/** The summary of the samples of one track. */
struct TrackStats {
  int sampleCount = 0;
  int keyFrameCount = 0;
  int64_t firstTimeUs = -1;
  int64_t lastTimeUs = -1;
  int64_t totalBytes = 0;
  int64_t minimumTimeDeltaUs = -1;
};

std::string mediaTypeToString(const MediaType type) {
  switch (type) {
    case MediaType::kMediaTypeAudio: {
      return "audio";
    }
    case MediaType::kMediaTypeVideo: {
      return "video";
    }
    case MediaType::kMediaTypeSubtitle: {
      return "subtitle";
    }
    case MediaType::kMediaTypeImage: {
      return "image";
    }
    case MediaType::kMediaTypeMetadata: {
      return "metadata";
    }
    case MediaType::kMediaTypeDefault: {
      return "unknown";
    }
    default: {
      return "invalid";
    }
  }
}

std::string describeSampleFlags(const uint32_t flags) {
  std::string description;
  const auto append = [&description](const char* text) {
    if (!description.empty()) {
      description.append("|");
    }
    description.append(text);
  };
  if ((flags & AVSample::kFlagKeyFrame) != 0) {
    append("key");
  }
  if ((flags & AVSample::kFlagCodecConfig) != 0) {
    append("config");
  }
  if ((flags & AVSample::kFlagFirstSample) != 0) {
    append("first");
  }
  if ((flags & AVSample::kFlagLastSample) != 0) {
    append("last");
  }
  if ((flags & AVSample::kFlagDiscard) != 0) {
    append("discard");
  }
  if ((flags & AVSample::kFlagEndOfStream) != 0) {
    append("eos");
  }
  if (description.empty()) {
    description = "-";
  }
  return description;
}

void printFormat(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    std::cout << "    format:  <unknown>\n";
    return;
  }
  std::cout << "    mime:    " << format->getMimeType() << "\n";
  if (!format->getContainerMimeType().empty()) {
    std::cout << "    container: " << format->getContainerMimeType() << "\n";
  }
  if (!format->getCodecs().empty()) {
    std::cout << "    codecs:  " << format->getCodecs() << "\n";
  }
  if (format->isAudio()) {
    std::cout << "    audio:   " << format->getSampleRate() << " Hz, "
              << format->getChannelCount() << " channel(s)\n";
  }
  if (format->isVideo()) {
    std::cout << "    video:   " << format->getWidth() << "x" << format->getHeight();
    if (format->getFrameRate() != MediaFormat::NO_VALUE) {
      std::cout << " @ " << format->getFrameRate() << " fps";
    }
    std::cout << "\n";
  }
  if (format->getDurationUs() > 0) {
    std::cout << "    duration: " << (format->getDurationUs() / 1000) << " ms\n";
  }
  if (format->getBitrate() != MediaFormat::NO_VALUE) {
    std::cout << "    bitrate: " << (format->getBitrate() / 1000) << " kbit/s\n";
  }
  const auto& csdList = format->csdList();
  for (size_t i = 0; i < csdList.size(); ++i) {
    if (csdList[i].empty()) {
      continue;
    }
    std::cout << "    csd[" << i << "]: " << csdList[i].size() << " bytes:";
    const size_t maximum = std::min<size_t>(csdList[i].size(), 16);
    for (size_t j = 0; j < maximum; ++j) {
      std::cout << " " << std::hex << static_cast<int>(csdList[i][j]) << std::dec;
    }
    if (csdList[i].size() > maximum) {
      std::cout << " ...";
    }
    std::cout << "\n";
  }
}

void printUsage(const char* const program) {
  std::cout << "Usage: " << program << " <media file> [options]\n"
            << "Options:\n"
            << "  --max-samples <count>  Stop after the given number of samples. Defaults to 16.\n"
            << "  --time-us <time>       Start at the given time, in microseconds.\n"
            << "  --file-type <type>     A hint of the container type, see coral::FileTypes.\n"
            << "  --quiet                Only print the summary, no sample detail.\n";
}

} // namespace anonymous

int main(const int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return EXIT_FAILURE;
  }

  std::string uri;
  int maxSamples = 16;
  int64_t seekTimeUs = -1;
  int fileType = -1;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--max-samples" && i + 1 < argc) {
      maxSamples = std::atoi(argv[++i]);
    } else if (argument == "--time-us" && i + 1 < argc) {
      seekTimeUs = std::atoll(argv[++i]);
    } else if (argument == "--file-type" && i + 1 < argc) {
      fileType = std::atoi(argv[++i]);
    } else if (argument == "--quiet") {
      quiet = true;
    } else if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return EXIT_SUCCESS;
    } else if (uri.empty()) {
      uri = argument;
    } else {
      std::cerr << "Unexpected argument: " << argument << "\n";
      printUsage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (uri.empty()) {
    printUsage(argv[0]);
    return EXIT_FAILURE;
  }

  if (fileType < 0) {
    fileType = FileTypes::inferFileTypeFromUrl(uri);
  }

  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(uri)) {
    std::cerr << "Unable to open " << uri << "\n";
    return EXIT_FAILURE;
  }

  auto reader = std::unique_ptr<AVReader>(
    new AVReader(dataSource, std::unique_ptr<AVExtractorsFactory>(
                               new AVDefaultExtractorsFactory(fileType))));

  std::map<int, TrackStats> stats;
  std::map<int, std::vector<AVSample>> retained;

  // The consumer runs on the thread that drives the reader. It only accumulates statistics and, up
  // to the requested limit, the samples themselves.
  const AVSampleConsumer consume = [&](const AVSample& sample) {
    if (sample.isEndOfStream()) {
      return;
    }
    auto& trackStats = stats[sample.trackIndex];
    ++trackStats.sampleCount;
    if (sample.isKeyFrame()) {
      ++trackStats.keyFrameCount;
    }
    if (trackStats.lastTimeUs >= 0) {
      const int64_t delta = sample.timeUs - trackStats.lastTimeUs;
      if (delta > 0 && (trackStats.minimumTimeDeltaUs < 0 || delta < trackStats.minimumTimeDeltaUs)) {
        trackStats.minimumTimeDeltaUs = delta;
      }
    } else {
      trackStats.firstTimeUs = sample.timeUs;
    }
    trackStats.lastTimeUs = sample.timeUs;
    trackStats.totalBytes += static_cast<int64_t>(sample.size());

    if (!quiet && maxSamples > 0) {
      auto& trackSamples = retained[sample.trackIndex];
      if (static_cast<int>(trackSamples.size()) < maxSamples) {
        // The buffer of the sample is reference counted, keeping the shared pointer is enough.
        trackSamples.push_back(AVSample(sample.trackIndex, sample.timeUs, sample.flags, sample.data));
      }
    }
  };

  if (!reader->prepare()) {
    std::cerr << "Unable to read " << uri << ": " << reader->getLastError() << "\n";
    return EXIT_FAILURE;
  }

  // The track consumers are installed once the tracks are known. The format of an audio container
  // is published by the extractor when it reads the header of the stream, i.e. during the first
  // read, therefore it is printed after the first sample was consumed.
  const auto trackIds = reader->getTrackIds();
  for (const int trackId : trackIds) {
    reader->setTrackConsumer(trackId, consume);
  }

  std::cout << "File:      " << uri << "\n";
  std::cout << "File type: " << fileType << "\n";
  const auto seekMap = reader->getSeekMap();
  if (seekMap != nullptr) {
    if (seekMap->getDurationUs() != coral::Definitions::kTimeUnset) {
      std::cout << "Duration:  " << (seekMap->getDurationUs() / 1000) << " ms\n";
    }
    std::cout << "Seekable:  " << (seekMap->isSeekable() ? "yes" : "no") << "\n";
  }
  std::cout << "Tracks:    " << trackIds.size() << "\n";

  if (seekTimeUs >= 0) {
    if (!reader->seek(-1, seekTimeUs)) {
      std::cerr << "Unable to seek to " << seekTimeUs << " us\n";
      return EXIT_FAILURE;
    }
  }

  int result = AVReader::kResultContinue;
  int iterations = 0;
  while (result == AVReader::kResultContinue && iterations++ < 10000000) {
    result = reader->read();
  }
  if (result == AVReader::kResultError) {
    std::cerr << "Read error: " << reader->getLastError() << "\n";
    return EXIT_FAILURE;
  }

  for (const int trackId : trackIds) {
    const auto track = reader->getTrack(trackId);
    std::cout << "  Track " << trackId << " ("
              << mediaTypeToString(track != nullptr ? track->getType()
                                                    : MediaType::kMediaTypeInvalid)
              << ")\n";
    printFormat(reader->getTrackFormat(trackId));
  }
  if (reader->getDurationUs() != coral::Definitions::kTimeUnset) {
    std::cout << "Duration:  " << (reader->getDurationUs() / 1000) << " ms\n";
  }

  std::cout << "\nSummary\n";
  for (const auto& entry : stats) {
    const auto& trackStats = entry.second;
    std::cout << "  Track " << entry.first << ": " << trackStats.sampleCount << " samples, "
              << trackStats.keyFrameCount << " key frames, " << trackStats.totalBytes << " bytes";
    if (trackStats.minimumTimeDeltaUs > 0 && trackStats.sampleCount > 1) {
      const double sampleRate = 1000000.0 / static_cast<double>(trackStats.minimumTimeDeltaUs);
      std::cout << ", ~" << sampleRate << " samples/s";
    }
    std::cout << "\n";
  }

  if (!quiet) {
    for (const auto& entry : retained) {
      std::cout << "\nSamples of track " << entry.first << ":\n";
      for (size_t i = 0; i < entry.second.size(); ++i) {
        const auto& sample = entry.second[i];
        std::cout << "  #" << i << " timeUs=" << sample.timeUs << " size=" << sample.size()
                  << " flags=" << describeSampleFlags(sample.flags) << "\n";
      }
      const auto& trackStats = stats[entry.first];
      if (trackStats.sampleCount > static_cast<int>(entry.second.size())) {
        std::cout << "  ... " << (trackStats.sampleCount - static_cast<int>(entry.second.size()))
                  << " more\n";
      }
    }
  }

  return EXIT_SUCCESS;
}
