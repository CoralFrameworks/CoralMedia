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

#include <extractor/audio/MpegAudio.h>

#include <media/Types.h>

namespace coral {

namespace {

constexpr int kSyncBits = 0x7FF;

/**
 * The bitrates of the MPEG-1 layer 1, in bits per second.
 */
constexpr int kBitrateV1L1[] = {0,  32,  64,  96,  128, 160, 192, 224,
                                256, 288, 320, 352, 384, 416, 448};

/**
 * The bitrates of the MPEG-1 layer 2, in bits per second.
 */
constexpr int kBitrateV1L2[] = {0,  32,  48,  56,  64,  80,  96,  112,
                                128, 160, 192, 224, 256, 320, 384};

/**
 * The bitrates of the MPEG-1 layer 3, in bits per second.
 */
constexpr int kBitrateV1L3[] = {0,  32,  40,  48,  56,  64,  80,  96,
                                112, 128, 160, 192, 224, 256, 320};

/**
 * The bitrates of the MPEG-2 and MPEG-2.5 layer 1, in bits per second.
 */
constexpr int kBitrateV2L1[] = {0,  32,  48,  56,  64,  80,  96,  112,
                                128, 144, 160, 176, 192, 224, 256};

/**
 * The bitrates of the MPEG-2 and MPEG-2.5 layer 2 and layer 3, in bits per second.
 */
constexpr int kBitrateV2L2[] = {0, 8,  16,  24,  32,  40,  48,  56,
                                64, 80, 96, 112, 128, 144, 160};

/**
 * The sample rates of the MPEG-1 streams, in Hz.
 */
constexpr int kSampleRateV1[] = {44100, 48000, 32000};

/**
 * The sample rates of the MPEG-2 streams, in Hz.
 */
constexpr int kSampleRateV2[] = {22050, 24000, 16000};

/**
 * The sample rates of the MPEG-2.5 streams, in Hz.
 */
constexpr int kSampleRateV25[] = {11025, 12000, 8000};

} // namespace anonymous

bool AVMpegAudioHeader::setForHeaderData(const uint32_t header) {
  // The eleven sync bits are the most significant bits of the header.
  if ((header >> 21) != kSyncBits) {
    return false;
  }

  const int versionBits = static_cast<int>((header >> 19) & 0x3);
  if (versionBits == 1) {
    // Reserved.
    return false;
  }
  switch (versionBits) {
    case 0: {
      version = Version::kVersion25;
      break;
    }
    case 2: {
      version = Version::kVersion2;
      break;
    }
    default: {
      version = Version::kVersion1;
      break;
    }
  }

  const int layerBits = static_cast<int>((header >> 17) & 0x3);
  if (layerBits == 0) {
    // Reserved.
    return false;
  }
  layer = 4 - layerBits;

  protectionAbsent = ((header >> 16) & 0x1) == 1;

  const int bitrateIndex = static_cast<int>((header >> 12) & 0xF);
  if (bitrateIndex == 0) {
    // A free format frame, the bitrate is not in the header.
    bitrate = -1;
  } else if (bitrateIndex == 0xF) {
    return false;
  } else if (version == Version::kVersion1) {
    if (layer == 1) {
      bitrate = kBitrateV1L1[bitrateIndex] * 1000;
    } else if (layer == 2) {
      bitrate = kBitrateV1L2[bitrateIndex] * 1000;
    } else {
      bitrate = kBitrateV1L3[bitrateIndex] * 1000;
    }
  } else {
    bitrate = (layer == 1 ? kBitrateV2L1[bitrateIndex] : kBitrateV2L2[bitrateIndex]) * 1000;
  }

  const int sampleRateIndex = static_cast<int>((header >> 10) & 0x3);
  if (sampleRateIndex == 3) {
    // Reserved.
    return false;
  }
  switch (version) {
    case Version::kVersion1: {
      sampleRate = kSampleRateV1[sampleRateIndex];
      break;
    }
    case Version::kVersion2: {
      sampleRate = kSampleRateV2[sampleRateIndex];
      break;
    }
    default: {
      sampleRate = kSampleRateV25[sampleRateIndex];
      break;
    }
  }

  padding = ((header >> 9) & 0x1) == 1;
  channelMode = static_cast<int>((header >> 6) & 0x3);
  channels = channelMode == 3 ? 1 : 2;
  return true;
}

int AVMpegAudioHeader::getFrameSize() const {
  if (bitrate <= 0 || sampleRate <= 0) {
    return kMaxFrameSize;
  }
  if (layer == 1) {
    const int frameSize = (12 * bitrate / sampleRate + (padding ? 1 : 0)) * 4;
    return frameSize > 0 ? frameSize : kMaxFrameSize;
  }
  const int coefficient = layer == 3 && version != Version::kVersion1 ? 72 : 144;
  const int frameSize = coefficient * bitrate / sampleRate + (padding ? 1 : 0);
  return frameSize > 0 ? frameSize : kMaxFrameSize;
}

int AVMpegAudioHeader::getSamplesPerFrame() const {
  if (layer == 1) {
    return 384;
  }
  if (layer == 2) {
    return 1152;
  }
  return version == Version::kVersion1 ? 1152 : 576;
}

std::string AVMpegAudioHeader::getMimeType() const {
  if (layer == 3) {
    return MimeTypes::AUDIO_MPEG;
  }
  if (layer == 2) {
    return MimeTypes::AUDIO_MPEG_L2;
  }
  return MimeTypes::AUDIO_MPEG_L1;
}

} // namespace coral
