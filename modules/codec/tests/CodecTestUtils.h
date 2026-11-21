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

#ifndef CORAL_CODEC_TESTS_CODEC_TEST_UTILS_H
#define CORAL_CODEC_TESTS_CODEC_TEST_UTILS_H

#include <codec/AudioFileReader.h>
#include <codec/AudioFileWriter.h>
#include <codec/MediaCodec.h>
#include <codec/MediaCodecFactory.h>
#include <codec/MediaExtractor.h>

#include <media/Format.h>
#include <media/PixelBuffer.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace coral::codec::test {

/**
 * The encoded packet which is collected from an encoder or an extractor.
 */
struct EncodedPacket {
  std::vector<uint8_t> data;
  int64_t timeUs = 0;
  bool keyFrame = false;
};

/**
 * Collects the tracks and the samples which are extracted by an AVMediaExtractor.
 */
class CollectingExtractorOutput final : public AVMediaExtractor::Output {
public:
  struct TrackData {
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    std::shared_ptr<MediaFormat> format;
    int64_t sampleCount = 0;
    int64_t byteCount = 0;
    int64_t firstTimeUs = Definitions::kTimeUnset;
    int64_t lastTimeUs = Definitions::kTimeUnset;
    int64_t keyFrameCount = 0;
    std::vector<EncodedPacket> packets;
  };

  class TrackCollector final : public AVTrackOutput::Consumer {
  public:
    explicit TrackCollector(TrackData* data) noexcept
      : mData(data) {
    }

    void setFormat(const std::shared_ptr<MediaFormat>& format) override {
      mData->format = format;
    }

    void consume(const AVSample& sample) override {
      mData->sampleCount++;
      mData->byteCount += static_cast<int64_t>(sample.size());
      if (mData->firstTimeUs == Definitions::kTimeUnset) {
        mData->firstTimeUs = sample.timeUs;
      }
      mData->lastTimeUs = sample.timeUs;
      if (sample.isKeyFrame()) {
        mData->keyFrameCount++;
      }
      EncodedPacket packet;
      packet.data.assign(sample.array(), sample.array() + sample.size());
      packet.timeUs = sample.timeUs;
      packet.keyFrame = sample.isKeyFrame();
      mData->packets.push_back(std::move(packet));
    }

  private:
    TrackData* mData;
  };

  std::shared_ptr<AVTrackOutput> track(const int trackId, const MediaType type,
    AVTrackOutput::SampleConsumer) override {
    // The consumer the extractor offers is not used: the platform extractor delivers the samples
    // through the track output, which is bound to the collector of this output.
    auto& data = tracks[trackId];
    data.mediaType = type;
    auto output = std::make_shared<TrackCollector>(&data);
    trackOutputs[trackId] = output;
    return std::make_shared<AVTrackOutput>(trackId, output);
  }

  std::shared_ptr<AVTrackOutput> track(const int trackId, AVTrackOutput::SampleConsumer consumer) override {
    // The media type of the track is not known, the samples are collected like the ones of the
    // tracks whose type was reported.
    return track(trackId, MediaType::kMediaTypeDefault, std::move(consumer));
  }

  void endTracks() override {
    endTracksCalled = true;
  }

  void seekMap(std::shared_ptr<AVSeekMap> map) override {
    mSeekMap = std::move(map);
  }

  [[nodiscard]] size_t trackCount() const {
    return tracks.size();
  }

  [[nodiscard]] const TrackData* firstTrackOfType(const MediaType type) const {
    for (const auto& [id, data]: tracks) {
      if (data.mediaType == type) {
        return &data;
      }
    }
    return nullptr;
  }

  std::map<int, TrackData> tracks;
  std::map<int, std::shared_ptr<TrackCollector>> trackOutputs;
  std::shared_ptr<AVSeekMap> mSeekMap;
  bool endTracksCalled = false;
};

/**
 * Creates a unique file path in the temporary directory of the process.
 */
inline std::string tempFilePath(const std::string& name) {
  static int counter = 0;
  const auto directory = std::filesystem::temp_directory_path() / "coral-codec-tests";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto file = directory / (std::to_string(++counter) + "-" + name);
  std::filesystem::remove(file, error);
  return file.string();
}

/**
 * Generates an interleaved 16 bit pcm signal of the given frequency.
 */
inline std::vector<int16_t> generatePcm(const int sampleRate,
  const int channelCount,
  const int frameCount,
  const double frequency = 440.0) {
  constexpr double PI = 3.14159265358979323846;
  std::vector<int16_t> pcm(static_cast<size_t>(frameCount) * channelCount);
  for (int frame = 0; frame < frameCount; ++frame) {
    const auto value = static_cast<int16_t>(
      std::sin(2.0 * PI * frequency * frame / sampleRate) * 12000.0);
    for (int channel = 0; channel < channelCount; ++channel) {
      pcm[static_cast<size_t>(frame) * channelCount + channel] = value;
    }
  }
  return pcm;
}

/**
 * Creates an i420 frame with a moving gradient.
 */
inline std::shared_ptr<I420PixelBuffer> generateI420Frame(const int width, const int height,
  const int index) {
  auto frame = std::make_shared<I420PixelBuffer>(width, height);
  for (int row = 0; row < height; ++row) {
    auto* rowData = frame->mutableDataY() + static_cast<ptrdiff_t>(row) * frame->strideY();
    for (int column = 0; column < width; ++column) {
      rowData[column] = static_cast<uint8_t>((column + row + index * 3) & 0xFF);
    }
  }
  for (int row = 0; row < frame->chromaHeight(); ++row) {
    auto* u = frame->mutableDataU() + static_cast<ptrdiff_t>(row) * frame->strideU();
    auto* v = frame->mutableDataV() + static_cast<ptrdiff_t>(row) * frame->strideV();
    for (int column = 0; column < frame->chromaWidth(); ++column) {
      u[column] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
      v[column] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
    }
  }
  return frame;
}

/**
 * Reads all frames of the reader until the end of the stream.
 */
inline int64_t readAllFrames(AudioFileReader* reader, Error** outError) {
  const auto bytesPerFrame = static_cast<int64_t>(
    MediaUtils::getBytesPerFrame(reader->getSampleFormat(),
      static_cast<uint32_t>(reader->getChannelCount())));
  if (bytesPerFrame <= 0) {
    return -1;
  }
  std::vector<uint8_t> buffer(static_cast<size_t>(bytesPerFrame) * 4096);
  int64_t frames = 0;
  while (true) {
    const auto read = reader->readIntoBuffer(buffer.data(), static_cast<int64_t>(buffer.size()),
      outError);
    if (read <= 0) {
      break;
    }
    frames += read;
  }
  return frames;
}

/**
 * Decodes the packets with the decoder and returns the number of the decoded sample frames.
 */
inline int64_t decodePackets(AudioDecoder* decoder, const std::vector<EncodedPacket>& packets,
  int64_t* outBytes = nullptr) {
  int64_t frames = 0;
  int64_t bytes = 0;

  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        break;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      if (!output->shouldBeSkipped) {
        const auto bytesPerFrame = static_cast<int64_t>(
          MediaUtils::getBytesPerFrame(output->getConfiguration().sampleFormat,
            output->getConfiguration().channelCount));
        if (bytesPerFrame > 0) {
          frames += static_cast<int64_t>(output->size()) / bytesPerFrame;
        }
      }
      bytes += static_cast<int64_t>(output->size());
      output->release();
    }
    return false;
  };

  for (const auto& packet: packets) {
    int guard = 0;
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }

  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }
  if (outBytes != nullptr) {
    *outBytes = bytes;
  }
  return frames;
}

} // namespace coral::codec::test

#endif //CORAL_CODEC_TESTS_CODEC_TEST_UTILS_H
