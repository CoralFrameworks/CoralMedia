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
 * A demo of the audio pipeline: a sine signal is encoded into an m4a file by the platform audio
 * file writer, the file is demuxed by the platform extractor afterwards and the extracted packets
 * are decoded by the platform audio decoder.
 *
 * usage: codec_audio_transcode [output file]
 */

#include <codec/AudioFileReader.h>
#include <codec/AudioFileWriter.h>
#include <codec/MediaCodecFactory.h>
#include <codec/MediaExtractor.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace coral;

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNEL_COUNT = 2;
constexpr int DURATION_SECONDS = 2;
constexpr int CHUNK_FRAMES = 1024;
constexpr double PI = 3.14159265358979323846;

std::vector<int16_t> generateSine(const int frameCount) {
  std::vector<int16_t> pcm(static_cast<size_t>(frameCount) * CHANNEL_COUNT);
  for (int frame = 0; frame < frameCount; ++frame) {
    const auto value = static_cast<int16_t>(
      std::sin(2.0 * PI * 440.0 * frame / SAMPLE_RATE) * 12000.0);
    for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
      pcm[static_cast<size_t>(frame) * CHANNEL_COUNT + channel] = value;
    }
  }
  return pcm;
}

bool writeFile(const std::string& path, const std::vector<int16_t>& pcm) {
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, SAMPLE_RATE, CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  format->setAverageBitrate(128000);

  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC, format);
  if (writer == nullptr) {
    printf("the platform does not provide an aac encoder\n");
    return false;
  }
  Error* outError = nullptr;
  if (!writer->open(&outError)) {
    printf("failed to open the output file: %s\n",
      outError != nullptr ? outError->toString().c_str() : "");
    return false;
  }

  const int frameCount = static_cast<int>(pcm.size() / CHANNEL_COUNT);
  int written = 0;
  while (written < frameCount) {
    const auto frames = std::min(CHUNK_FRAMES, frameCount - written);
    if (writer->writeFromPCMBuffer(
      const_cast<int16_t*>(pcm.data()) + static_cast<size_t>(written) * CHANNEL_COUNT,
      frames, &outError) != frames) {
      printf("failed to write the samples\n");
      return false;
    }
    written += frames;
  }
  writer->close();
  printf("wrote %d frames into %s (%.1f kb)\n", frameCount, path.c_str(),
    std::filesystem::file_size(path) / 1024.0);
  return true;
}

struct Packet {
  std::vector<uint8_t> data;
  int64_t timeUs = 0;
  bool keyFrame = false;
};

class DemuxOutput final : public AVMediaExtractor::Output {
public:
  class TrackCollector final : public AVTrackOutput::Consumer {
  public:
    void setFormat(const std::shared_ptr<MediaFormat>& newFormat) override {
      format = newFormat;
    }

    void consume(const AVSample& sample) override {
      packets.push_back({ std::vector<uint8_t>(sample.array(), sample.array() + sample.size()),
        sample.timeUs, sample.isKeyFrame() });
    }

    std::shared_ptr<MediaFormat> format;
    std::vector<Packet> packets;
  };

  std::shared_ptr<AVTrackOutput> track(const int id, const MediaType type,
    AVTrackOutput::SampleConsumer) override {
    // The consumer the extractor offers is not used: the platform extractor delivers the samples
    // through the track output, which is bound to the collector of this output.
    if (mTrackOutput == nullptr) {
      mTrack = std::make_shared<TrackCollector>();
      mTrackOutput = std::make_shared<AVTrackOutput>(id, mTrack);
    }
    return mTrackOutput;
  }

  std::shared_ptr<AVTrackOutput> track(const int id, AVTrackOutput::SampleConsumer consumer) override {
    // The media type of the track is not known, the samples are collected like the ones of the
    // tracks whose type was reported.
    return track(id, MediaType::kMediaTypeDefault, std::move(consumer));
  }

  void endTracks() override {
  }

  void seekMap(std::shared_ptr<AVSeekMap>) override {
  }

  std::shared_ptr<TrackCollector> mTrack;
  std::shared_ptr<AVTrackOutput> mTrackOutput;
};

int64_t decode(const std::shared_ptr<MediaFormat>& format, const std::vector<Packet>& packets) {
  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(format->getMimeType(), false);
  if (decoder == nullptr) {
    printf("no audio decoder is available for %s\n", format->getMimeType().c_str());
    return -1;
  }
  if (!decoder->configure(format)) {
    printf("failed to configure the audio decoder\n");
    return -1;
  }
  decoder->start();

  int64_t frames = 0;
  int64_t bytes = 0;
  const auto drain = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      const auto bytesPerFrame = static_cast<int64_t>(
        MediaUtils::getBytesPerFrame(output->getConfiguration().sampleFormat,
          output->getConfiguration().channelCount));
      if (bytesPerFrame > 0) {
        frames += static_cast<int64_t>(output->size()) / bytesPerFrame;
      }
      bytes += static_cast<int64_t>(output->size());
      output->release();
    }
  };

  for (const auto& packet: packets) {
    while (true) {
      auto input = decoder->dequeueInputBuffer();
      if (input != nullptr) {
        input->ensureSpaceForWrite(packet.data.size());
        const auto data = input->data();
        data->position(0);
        data->put(packet.data.data(), packet.data.size());
        data->flip();
        input->timeUs(packet.timeUs);
        if (packet.keyFrame) {
          input->addFlag(AVBufferFlags::kKeyFrame);
        }
        decoder->queueInputBuffer(input);
        break;
      }
      if (drain()) {
        break;
      }
    }
    drain();
  }

  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drain() && ++guard < 1000) {
  }
  decoder->stop();
  decoder->release();

  printf("decoded %lld frames (%lld kb) of pcm, output format: %s\n",
    static_cast<long long>(frames), static_cast<long long>(bytes / 1024),
    decoder->getOutputFormat() != nullptr ? decoder->getOutputFormat()->toString().c_str() : "n/a");
  return frames;
}

} // namespace anonymous

int main(int argc, char** argv) {
  std::string path;
  if (argc > 1) {
    path = argv[1];
  } else {
    path = (std::filesystem::temp_directory_path() / "coral-audio-transcode.m4a").string();
  }

  const int frameCount = SAMPLE_RATE * DURATION_SECONDS;
  const auto pcm = generateSine(frameCount);
  if (!writeFile(path, pcm)) {
    return 1;
  }

  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  if (extractor == nullptr) {
    printf("the file is not supported by the platform extractor\n");
    return 1;
  }
  auto output = std::make_shared<DemuxOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }
  if (output->mTrack == nullptr || output->mTrack->format == nullptr) {
    printf("no audio track was found in the file\n");
    return 1;
  }
  printf("extracted %zu packets, duration: %.3f s\n", output->mTrack->packets.size(),
    extractor->getDurationUs() / 1000000.0);

  const auto frames = decode(output->mTrack->format, output->mTrack->packets);
  extractor->clear();
  if (frames < 0) {
    return 1;
  }
  printf("input frames: %d, decoded frames: %lld\n", frameCount, static_cast<long long>(frames));
  return 0;
}
