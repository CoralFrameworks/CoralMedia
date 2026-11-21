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
 * Validates a CoralMedia checkout end to end, through the framework's public API only: it encodes
 * synthetic audio and video with the platform encoders, muxes the encoded tracks with the `muxer`
 * module, demuxes the file it wrote with the `extractor` module and decodes the demuxed tracks with
 * the platform decoders. An optional `--input <file>` runs the demux and decode stages on a real
 * media file as well.
 *
 * Usage:
 *   coralmedia_validate [--input <file>] [--workdir <dir>] [--keep]
 *
 * Exit code: 0 when no stage failed, 1 when one did, 77 when nothing could be validated (no platform
 * codec and no input file) -- CTest treats 77 as skipped.
 */

#include <codec/MediaCodecFactory.h>
#include <codec/MediaMuxer.h>

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>

#include <media/Format.h>
#include <media/PixelBuffer.h>
#include <media/Utilities.h>

#include <muxer/Muxer.h>
#include <muxer/MuxerFactory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace coral;

namespace {

// ==================================================================================================
// Content the encode stage produces
// ==================================================================================================
constexpr int VIDEO_WIDTH = 640;
constexpr int VIDEO_HEIGHT = 480;
constexpr int VIDEO_FRAME_COUNT = 30;
constexpr int VIDEO_FRAME_RATE = 30;
constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNEL_COUNT = 2;
constexpr int AUDIO_FRAME_COUNT = AUDIO_SAMPLE_RATE; // one second
constexpr int AUDIO_CHUNK_FRAMES = 1024;
constexpr double PI = 3.14159265358979323846;

/** One encoded or demuxed sample. */
struct Packet {
  std::vector<uint8_t> data;
  int64_t timeUs = 0;
  bool keyFrame = false;
};

/** The encoded packets and the format the encoder reported for them. */
struct EncodedTrack {
  std::shared_ptr<MediaFormat> format;
  std::vector<Packet> packets;
};

/** One track of a demuxed file. */
struct DemuxedTrack {
  int id = 0;
  MediaType type = MediaType::kMediaTypeDefault;
  std::shared_ptr<MediaFormat> format;
  std::vector<Packet> packets;
};

/** The tracks and timing of a demuxed file. */
struct DemuxedFile {
  std::filesystem::path path;
  int64_t durationUs = -1;
  std::string extractor;
  std::vector<DemuxedTrack> tracks;
};

// ==================================================================================================
// Stage results
// ==================================================================================================
enum class Outcome {
  kPass,
  kSkip,
  kFail,
};

struct StageResult {
  Outcome outcome = Outcome::kSkip;
  std::string detail;
};

const char* outcomeName(const Outcome outcome) {
  switch (outcome) {
    case Outcome::kPass: {
      return "PASS";
    }
    case Outcome::kSkip: {
      return "SKIP";
    }
    case Outcome::kFail: {
      return "FAIL";
    }
  }
  return "?";
}

std::string describeFormat(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return "unknown format";
  }
  std::string text = format->getMimeType();
  if (format->isVideo()) {
    text += " " + std::to_string(format->getWidth()) + "x" + std::to_string(format->getHeight());
  } else if (format->isAudio()) {
    text += " " + std::to_string(format->getSampleRate()) + " Hz, " +
            std::to_string(format->getChannelCount()) + " ch";
  }
  return text;
}

// ==================================================================================================
// Encode: synthetic content through the platform encoders
// ==================================================================================================
StageResult encodeAudio(EncodedTrack& track) {
  auto inputFormat = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW, AUDIO_SAMPLE_RATE,
    AUDIO_CHANNEL_COUNT);
  inputFormat->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);
  inputFormat->setSampleFormat(AudioSampleFormat::kInt16);

  auto encoder = CreateBuiltinAudioCodecFactory()->createEncoder(
    AudioFormatType::kAudioFormatTypeMpeg4AAC, CodecManufacturer::kHardware);
  if (encoder == nullptr) {
    return {Outcome::kSkip, "the platform has no AAC encoder"};
  }
  if (!encoder->configure(inputFormat)) {
    return {Outcome::kSkip, "the AAC encoder cannot be configured for 48 kHz stereo"};
  }
  encoder->start();

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        track.format = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      track.packets.push_back({std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
        packet->timeUs(), packet->isKeyFrame()});
    }
  };

  const auto bytesPerFrame = AUDIO_CHANNEL_COUNT * static_cast<int>(sizeof(int16_t));
  std::vector<int16_t> chunk(static_cast<size_t>(AUDIO_CHUNK_FRAMES) * AUDIO_CHANNEL_COUNT);
  int offset = 0;
  while (offset < AUDIO_FRAME_COUNT) {
    auto input = encoder->dequeueInputBuffer();
    if (input == nullptr) {
      drain();
      continue;
    }
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, AUDIO_FRAME_COUNT - offset);
    for (int frame = 0; frame < frames; ++frame) {
      const auto value = static_cast<int16_t>(
        std::sin(2.0 * PI * 440.0 * (offset + frame) / AUDIO_SAMPLE_RATE) * 12000.0);
      for (int channel = 0; channel < AUDIO_CHANNEL_COUNT; ++channel) {
        chunk[static_cast<size_t>(frame) * AUDIO_CHANNEL_COUNT + channel] = value;
      }
    }
    const auto size = static_cast<size_t>(frames) * bytesPerFrame;
    input->init(static_cast<int64_t>(offset) * Definitions::kMicrosPerSecond / AUDIO_SAMPLE_RATE,
      static_cast<int>(size));
    const auto data = input->data();
    data->position(0);
    data->put(reinterpret_cast<const uint8_t*>(chunk.data()), size);
    data->limit(size);
    encoder->queueInputBuffer(input);
    offset += frames;
    drain();
  }

  encoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !endOfStream; ++guard) {
    const auto count = track.packets.size();
    drain();
    if (track.packets.size() == count && guard > 4) {
      break;
    }
  }
  encoder->stop();
  encoder->release();

  if (track.packets.empty() || track.format == nullptr) {
    return {Outcome::kSkip, "the AAC encoder produced no packet"};
  }
  return {Outcome::kPass, std::to_string(track.packets.size()) + " packets, " +
                          describeFormat(track.format)};
}

StageResult encodeVideo(EncodedTrack& track) {
  auto inputFormat = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, VIDEO_WIDTH,
    VIDEO_HEIGHT);
  inputFormat->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
  inputFormat->setFrameRate(VIDEO_FRAME_RATE);
  inputFormat->setAverageBitrate(2000000);

  // The hardware encoder takes a D3D11 device, which a headless build machine does not have; the
  // software MFT is the portable path and what a validation run should fall back to.
  const auto create = [](const CodecManufacturer manufacturer) {
    return CreateBuiltinVideoCodecFactory()->createVideoEncoder(
      VideoCodecType::kVideoCodecTypeH264, manufacturer, nullptr);
  };
  const char* manufacturer = "hardware";
  auto encoder = create(CodecManufacturer::kHardware);
  if (encoder == nullptr || !encoder->configure(inputFormat, nullptr)) {
    manufacturer = "software";
    encoder = create(CodecManufacturer::kSoftware);
  }
  if (encoder == nullptr || !encoder->configure(inputFormat, nullptr)) {
    return {Outcome::kSkip, "the platform has no H.264 encoder"};
  }
  encoder->start();

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        track.format = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      track.packets.push_back({std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
        packet->timeUs(), packet->isKeyFrame()});
    }
  };

  for (int index = 0; index < VIDEO_FRAME_COUNT; ++index) {
    std::shared_ptr<AVPixelBuffer> input;
    int guard = 0;
    while (input == nullptr && guard++ < 1000) {
      input = encoder->dequeueInputBuffer();
      if (input == nullptr) {
        drain();
      }
    }
    if (input == nullptr) {
      encoder->stop();
      encoder->release();
      return {Outcome::kSkip, "the H.264 encoder did not accept the synthetic frames"};
    }

    // A moving gradient, written into whichever layout the encoder's input buffer uses.
    if (input->getFormat() == PixelFormat::kFormatI420) {
      auto i420 = input->asI420PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        auto* rowData = i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY();
        for (int column = 0; column < VIDEO_WIDTH; ++column) {
          rowData[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
        }
      }
      for (int row = 0; row < i420->chromaHeight(); ++row) {
        auto* u = i420->mutableDataU() + static_cast<ptrdiff_t>(row) * i420->strideU();
        auto* v = i420->mutableDataV() + static_cast<ptrdiff_t>(row) * i420->strideV();
        for (int column = 0; column < i420->chromaWidth(); ++column) {
          u[column] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
          v[column] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
        }
      }
    } else {
      auto nv12 = input->asNV12PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        auto* rowData = nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY();
        for (int column = 0; column < VIDEO_WIDTH; ++column) {
          rowData[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
        }
      }
      for (int row = 0; row < nv12->chromaHeight(); ++row) {
        auto* uv = nv12->mutableDataUV() + static_cast<ptrdiff_t>(row) * nv12->strideUV();
        for (int column = 0; column < nv12->chromaWidth(); ++column) {
          uv[column * 2] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
          uv[column * 2 + 1] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
        }
      }
    }

    input->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / VIDEO_FRAME_RATE);
    encoder->queueInputBuffer(input);
    drain();
  }

  encoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !endOfStream; ++guard) {
    const auto count = track.packets.size();
    drain();
    if (track.packets.size() == count && guard > 4) {
      break;
    }
  }
  encoder->stop();
  encoder->release();

  if (track.packets.empty() || track.format == nullptr) {
    return {Outcome::kSkip, "the H.264 encoder produced no packet"};
  }
  return {Outcome::kPass, std::string(manufacturer) + ": " + std::to_string(track.packets.size()) +
                          " packets, " + describeFormat(track.format)};
}

// ==================================================================================================
// Mux: the framework's own muxer, with the platform muxer as the fallback writer
// ==================================================================================================
StageResult muxWithModule(const std::filesystem::path& path, const EncodedTrack& audio,
  const EncodedTrack& video) {
  auto factory = muxer::createMuxerFactory();
  if (factory == nullptr) {
    return {Outcome::kSkip, "the framework has no muxer factory"};
  }

  muxer::Error error;
  const auto url = path.string();
  auto muxer = factory->create(url, muxer::Container::kMp4, muxer::MuxerOptions{}, &error);
  if (muxer == nullptr) {
    return {Outcome::kFail, "cannot open " + url + ": " + error.message};
  }

  const int videoTrack = video.format != nullptr ? muxer->addTrack(video.format) : -1;
  const int audioTrack = audio.format != nullptr ? muxer->addTrack(audio.format) : -1;
  if (videoTrack < 0 && audioTrack < 0) {
    return {Outcome::kSkip, "the MP4 muxer rejects the encoder formats: " + muxer->getError().message};
  }

  const auto write = [&](const int trackId, const EncodedTrack& track) {
    for (const auto& packet: track.packets) {
      muxer::SampleInfo info;
      info.presentationTimeUs = packet.timeUs;
      info.flags = packet.keyFrame ? static_cast<muxer::SampleFlags>(muxer::SampleFlag::kKeyFrame) : 0;
      if (!muxer->writeSample(trackId, packet.data.data(), packet.data.size(), info)) {
        return false;
      }
    }
    return true;
  };
  const bool written = (audioTrack >= 0 ? write(audioTrack, audio) : true) &&
                       (videoTrack >= 0 ? write(videoTrack, video) : true);
  if (!written || !muxer->finish()) {
    return {Outcome::kFail, "cannot write " + url + ": " + muxer->getError().message};
  }

  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code || size == 0) {
    return {Outcome::kFail, "the muxer wrote no data into " + url};
  }
  return {Outcome::kPass, std::to_string(size / 1024) + " kb into " + path.filename().string() +
                          " (muxer module, " + (videoTrack >= 0 ? "video" : "audio") +
                          (videoTrack >= 0 && audioTrack >= 0 ? "+audio" : "") + ")"};
}

StageResult muxWithPlatform(const std::filesystem::path& path, const EncodedTrack& audio,
  const EncodedTrack& video) {
  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  if (muxer == nullptr || !muxer->prepare(path.string(), MuxerFormat::kMuxerFormatMpeg4)) {
    return {Outcome::kFail, "the platform muxer cannot write " + path.string()};
  }

  const auto audioTrack = audio.format != nullptr ? muxer->addTrack(audio.format) :
                                                   AVMediaMuxer::kInvalidTrack;
  const auto videoTrack = video.format != nullptr ? muxer->addTrack(video.format) :
                                                   AVMediaMuxer::kInvalidTrack;
  if (audioTrack == AVMediaMuxer::kInvalidTrack && videoTrack == AVMediaMuxer::kInvalidTrack) {
    return {Outcome::kFail, "the platform muxer rejects both encoded tracks"};
  }
  muxer->start();

  const auto write = [&](const std::vector<Packet>& packets, const int streamIndex,
                       const MediaType mediaType) {
    for (const auto& packet: packets) {
      auto buffer = std::make_shared<AVPacketBuffer>(mediaType);
      buffer->ensureSpaceForWrite(packet.data.size());
      const auto data = buffer->data();
      data->position(0);
      data->put(packet.data.data(), packet.data.size());
      data->flip();
      buffer->timeUs(packet.timeUs);
      buffer->setStreamIndex(streamIndex);
      if (packet.keyFrame) {
        buffer->addFlag(AVBufferFlags::kKeyFrame);
      }
      muxer->writeFrame(buffer);
    }
  };
  if (audioTrack != AVMediaMuxer::kInvalidTrack) {
    write(audio.packets, audioTrack, MediaType::kMediaTypeAudio);
  }
  if (videoTrack != AVMediaMuxer::kInvalidTrack) {
    write(video.packets, videoTrack, MediaType::kMediaTypeVideo);
  }
  muxer->stop();
  muxer->release();

  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code || size == 0) {
    return {Outcome::kFail, "the platform muxer wrote no data into " + path.string()};
  }
  return {Outcome::kPass, std::to_string(size / 1024) + " kb into " + path.filename().string() +
                          " (platform muxer)"};
}

// ==================================================================================================
// Demux: the framework's own extractor
// ==================================================================================================
StageResult demuxFile(const std::filesystem::path& path, DemuxedFile& demuxed) {
  auto source = std::make_shared<AVFileDataSource>();
  if (!source->open(path.string())) {
    return {Outcome::kFail, "cannot open " + path.string()};
  }
  AVReader reader(source, AVExtractorsFactory::createDefault(-1, source->getLength()));
  if (!reader.prepare()) {
    return {Outcome::kFail, "the extractor cannot open " + path.string() + ": " +
                            reader.getLastError()};
  }

  demuxed.path = path;
  demuxed.durationUs = reader.getDurationUs();
  demuxed.extractor = reader.getSelectedExtractorName();

  std::map<int, size_t> indices;
  for (const int id: reader.getTrackIds()) {
    const auto track = reader.getTrack(id);
    DemuxedTrack entry;
    entry.id = id;
    entry.type = track != nullptr ? track->getType() : MediaType::kMediaTypeDefault;
    entry.format = reader.getTrackFormat(id);
    indices[id] = demuxed.tracks.size();
    demuxed.tracks.push_back(entry);
  }

  for (const auto& [id, index]: indices) {
    reader.setTrackConsumer(id, [&demuxed, index](const AVSample& sample) {
      if (sample.isEndOfStream() || sample.size() == 0) {
        return;
      }
      demuxed.tracks[index].packets.push_back(
        {std::vector<uint8_t>(sample.array(), sample.array() + sample.size()), sample.timeUs,
          sample.isKeyFrame()});
    });
  }

  int guard = 0;
  while (reader.read() == AVReader::kResultContinue && ++guard < 200000) {
  }

  size_t samples = 0;
  for (const auto& track: demuxed.tracks) {
    samples += track.packets.size();
  }
  if (demuxed.tracks.empty()) {
    return {Outcome::kFail, "no track was found in " + path.string()};
  }
  if (samples == 0) {
    return {Outcome::kFail, "the tracks of " + path.string() + " carry no sample"};
  }
  return {Outcome::kPass, std::to_string(demuxed.tracks.size()) + " tracks, " +
                          std::to_string(samples) + " samples, extractor " +
                          (demuxed.extractor.empty() ? "?" : demuxed.extractor)};
}

// ==================================================================================================
// Decode: the platform decoders
// ==================================================================================================
int64_t decodeVideo(const DemuxedTrack& track, std::string& detail) {
  auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  if (decoder == nullptr || track.format == nullptr || !decoder->configure(track.format, nullptr)) {
    detail = "the platform has no H.264 decoder for this track";
    return -1;
  }
  decoder->start();

  int64_t frames = 0;
  const auto drain = [&]() {
    while (true) {
      auto frame = decoder->dequeueOutputBuffer();
      if (frame == nullptr) {
        return false;
      }
      if (frame->isEndOfStream()) {
        frame->release();
        return true;
      }
      frames++;
      frame->release();
    }
  };

  for (const auto& packet: track.packets) {
    int guard = 0;
    while (guard++ < 1000) {
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
  for (int guard = 0; guard < 1000 && !drain(); ++guard) {
  }
  decoder->stop();
  decoder->release();

  detail = std::to_string(frames) + " video frames";
  return frames;
}

int64_t decodeAudio(const DemuxedTrack& track, std::string& detail) {
  if (track.format == nullptr) {
    detail = "the audio track has no format";
    return -1;
  }
  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(track.format->getMimeType(), false);
  if (decoder == nullptr || !decoder->configure(track.format)) {
    detail = "the platform has no decoder for " + track.format->getMimeType();
    return -1;
  }
  decoder->start();

  int64_t frames = 0;
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
      const auto bytesPerFrame = static_cast<int64_t>(MediaUtils::getBytesPerFrame(
        output->getConfiguration().sampleFormat, output->getConfiguration().channelCount));
      if (bytesPerFrame > 0) {
        frames += static_cast<int64_t>(output->size()) / bytesPerFrame;
      }
      output->release();
    }
  };

  for (const auto& packet: track.packets) {
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
  for (int guard = 0; guard < 1000 && !drain(); ++guard) {
  }
  decoder->stop();
  decoder->release();

  detail = std::to_string(frames) + " pcm frames";
  return frames;
}

StageResult decodeTracks(const DemuxedFile& file) {
  int64_t videoFrames = 0;
  int64_t audioFrames = 0;
  std::string videoDetail = "no video track";
  std::string audioDetail = "no audio track";
  bool sawDecoder = false;

  for (const auto& track: file.tracks) {
    if (track.format == nullptr || track.packets.empty()) {
      continue;
    }
    if (track.format->isVideo()) {
      std::string detail;
      const auto frames = decodeVideo(track, detail);
      videoDetail = detail;
      if (frames > 0) {
        videoFrames += frames;
        sawDecoder = true;
      }
    } else if (track.format->isAudio()) {
      std::string detail;
      const auto frames = decodeAudio(track, detail);
      audioDetail = detail;
      if (frames > 0) {
        audioFrames += frames;
        sawDecoder = true;
      }
    }
  }

  if (!sawDecoder) {
    return {Outcome::kSkip, videoDetail + "; " + audioDetail};
  }
  const auto detail = videoDetail + "; " + audioDetail;
  if (audioFrames == 0 && videoFrames == 0) {
    return {Outcome::kFail, detail};
  }
  return {Outcome::kPass, detail};
}

// ==================================================================================================
// Driver
// ==================================================================================================
struct Options {
  std::string input;
  std::filesystem::path workDir;
  bool keep = false;
};

bool parseOptions(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--input" && index + 1 < argc) {
      options.input = argv[++index];
    } else if (argument == "--workdir" && index + 1 < argc) {
      options.workDir = argv[++index];
    } else if (argument == "--keep") {
      options.keep = true;
    } else if (argument == "--help" || argument == "-h") {
      return false;
    } else {
      std::printf("unknown argument: %s\n", argument.c_str());
      return false;
    }
  }
  if (options.workDir.empty()) {
    options.workDir = std::filesystem::temp_directory_path() / "coralmedia-validate";
  }
  return true;
}

void report(const char* stage, const StageResult& result) {
  std::printf("[%s] %-7s %s\n", outcomeName(result.outcome), stage, result.detail.c_str());
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    std::printf("usage: coralmedia_validate [--input <file>] [--workdir <dir>] [--keep]\n");
    return 1;
  }

  std::error_code error;
  std::filesystem::create_directories(options.workDir, error);
  if (error) {
    std::printf("cannot create the work directory %s: %s\n", options.workDir.string().c_str(),
      error.message().c_str());
    return 1;
  }

  int passed = 0;
  int skipped = 0;
  int failed = 0;
  const auto count = [&](const StageResult& result) {
    switch (result.outcome) {
      case Outcome::kPass: {
        passed++;
        break;
      }
      case Outcome::kSkip: {
        skipped++;
        break;
      }
      case Outcome::kFail: {
        failed++;
        break;
      }
    }
  };

  // ---- encode ----
  EncodedTrack audio;
  EncodedTrack video;
  const auto audioEncode = encodeAudio(audio);
  report("encode-audio", audioEncode);
  count(audioEncode);
  const auto videoEncode = encodeVideo(video);
  report("encode-video", videoEncode);
  count(videoEncode);

  // ---- mux ----
  const auto outputPath = options.workDir / "coralmedia-validate.mp4";
  std::filesystem::remove(outputPath, error);
  auto mux = (audio.format != nullptr || video.format != nullptr)
               ? muxWithModule(outputPath, audio, video)
               : StageResult{Outcome::kSkip, "nothing was encoded"};
  if (mux.outcome != Outcome::kPass) {
    // The platform muxer is the fallback writer, so that the demux and decode stages still have a
    // real file even when the module muxer does not accept the encoder formats.
    const auto fallback = (audio.format != nullptr || video.format != nullptr)
                            ? muxWithPlatform(outputPath, audio, video)
                            : StageResult{Outcome::kSkip, "nothing was encoded"};
    if (fallback.outcome == Outcome::kPass) {
      mux = StageResult{Outcome::kPass, mux.detail + "; fallback: " + fallback.detail};
    } else if (mux.outcome == Outcome::kSkip) {
      mux = fallback;
    }
  }
  report("mux", mux);
  count(mux);

  // ---- demux ----
  DemuxedFile demuxedOutput;
  auto demux = std::filesystem::exists(outputPath, error)
                 ? demuxFile(outputPath, demuxedOutput)
                 : StageResult{Outcome::kSkip, "the mux stage wrote no file"};
  if (demux.outcome == Outcome::kPass) {
    demux.detail = outputPath.filename().string() + ": " + demux.detail;
  }
  report("demux", demux);
  count(demux);

  // The demux and decode stages run on the caller's file as well, and that is the source the decode
  // stage prefers, because it carries real codec content rather than the synthetic gradient.
  DemuxedFile demuxedInput;
  StageResult inputDemux{Outcome::kSkip, "no --input file"};
  if (!options.input.empty()) {
    inputDemux = demuxFile(options.input, demuxedInput);
    if (inputDemux.outcome == Outcome::kPass) {
      inputDemux.detail = std::filesystem::path(options.input).filename().string() + ": " +
                          inputDemux.detail;
    }
    report("demux-input", inputDemux);
    count(inputDemux);
  }

  // ---- decode ----
  const DemuxedFile* decodeSource = !demuxedInput.tracks.empty() ? &demuxedInput :
                                    (!demuxedOutput.tracks.empty() ? &demuxedOutput : nullptr);
  auto decode = decodeSource != nullptr ? decodeTracks(*decodeSource)
                                        : StageResult{Outcome::kSkip, "there is no demuxed track"};
  if (decode.outcome != Outcome::kSkip && decodeSource != nullptr) {
    decode.detail = decodeSource->path.filename().string() + ": " + decode.detail;
  }
  report("decode", decode);
  count(decode);

  // ---- summary ----
  const auto total = passed + skipped + failed;
  std::printf("\n%zu stage(s): %d passed, %d skipped, %d failed\n",
    static_cast<size_t>(total), passed, skipped, failed);
  if (decodeSource != nullptr) {
    std::printf("decoded source: %s\n", decodeSource->path.string().c_str());
  }
  if (!options.keep && failed == 0) {
    std::filesystem::remove_all(options.workDir, error);
  } else {
    std::printf("outputs kept in %s\n", options.workDir.string().c_str());
  }

  if (failed > 0) {
    return 1;
  }
  if (passed == 0) {
    // Nothing could be validated: no platform codec and no input file.
    return 77;
  }
  return 0;
}
