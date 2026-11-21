/*
 * Copyright (C) 2023 The Coral Project
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

#include "MediaCodecUtils.h"

#include <media/Format.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <core/jni/android/AudioFormat.h>

#include <sys/system_properties.h>

#include <array>
#include <mutex>
#include <string>

namespace coral {

namespace {

uint32_t sApiLevel = 0;
std::once_flag sApiLevelOnceFlag;

int getAndroidApiLevel() {
  std::call_once(sApiLevelOnceFlag, []() {
    char sdkVersion[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdkVersion);
    sApiLevel = atoi(sdkVersion);
  });
  return sApiLevel;
}

} // namespace anonymous

ColorInfo::RangeType MediaCodecUtils::getColorRange(int color_range) {
  switch (color_range) {
    case COLOR_RANGE_FULL: {
      return ColorInfo::RangeType::kRangeFull;
    }
    case COLOR_RANGE_LIMITED: {
      return ColorInfo::RangeType::kRangeLimited;
    }
    default: {
      return ColorInfo::RangeType::kRangeUnknown;
    }
  }
}

ColorInfo::SpaceType MediaCodecUtils::getColorSpace(int color_space) {
  switch (color_space) {
    case COLOR_STANDARD_BT709: {
      return ColorInfo::SpaceType::kSpaceBT709;
    }
    case COLOR_STANDARD_BT601_PAL:
    case COLOR_STANDARD_BT601_NTSC: {
      return ColorInfo::SpaceType::kSpaceBT601;
    }
    case COLOR_STANDARD_BT2020: {
      return ColorInfo::SpaceType::kSpaceBT2020;
    }
    default: {
      return ColorInfo::SpaceType::kSpaceUnknown;
    }
  }
}

ColorInfo::TransferType MediaCodecUtils::getColorTransfer(int color_transfer) {
  switch (color_transfer) {
    case COLOR_TRANSFER_LINEAR: {
      return ColorInfo::TransferType::kTransferLinear;
    }
    case COLOR_TRANSFER_SRGB: {
      return ColorInfo::TransferType::kTransferSRGB;
    }
    case COLOR_TRANSFER_SDR_VIDEO: {
      return ColorInfo::TransferType::kTransferSDR;
    }
    case COLOR_TRANSFER_ST2084: {
      return ColorInfo::TransferType::kTransferST2084;
    }
    case COLOR_TRANSFER_HLG: {
      return ColorInfo::TransferType::kTransferHLG;
    }
    default: {
      return ColorInfo::TransferType::kTransferUnknown;
    }
  }
}

int MediaCodecUtils::getApiLevel() {
  return getAndroidApiLevel();
}

bool MediaCodecUtils::supportAudioDecoding(const AudioFormatType& codecType) {
  switch (codecType) {
    case AudioFormatType::kAudioFormatTypeAC3:
    case AudioFormatType::kAudioFormatTypeALaw:
    case AudioFormatType::kAudioFormatTypeAMR:
    case AudioFormatType::kAudioFormatTypeAMR_WB:
    case AudioFormatType::kAudioFormatTypeFlac:
    case AudioFormatType::kAudioFormatTypeMpeg3:
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
    case AudioFormatType::kAudioFormatTypeOpus:
    case AudioFormatType::kAudioFormatTypeVorbis:
    case AudioFormatType::kAudioFormatTypeLinearPCM: {
      return true;
    }
    default: {
      break;
    }
  }

  //!< TODO checks whether android.media.MediaCodecList supports the AudioFormatType

  return false;
}

bool MediaCodecUtils::supportAudioEncoding(const AudioFormatType& codecType) {
  if (codecType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    return true;
  }
  //!< todo checks whether android.media.MediaCodecList supports the AudioFormatType

  return false;
}

bool MediaCodecUtils::supportVideoDecoding(const VideoCodecType& codecType) {
  switch (codecType) {
    case VideoCodecType::kVideoCodecTypeH263:
    case VideoCodecType::kVideoCodecTypeH264:
    case VideoCodecType::kVideoCodecTypeHevc:
    case VideoCodecType::kVideoCodecTypeVP8:
    case VideoCodecType::kVideoCodecTypeVP9: {
      return true;
    }
    default: {
      break;
    }
  }
  //!< todo checks whether android.media.MediaCodecList supports the VideoCodecType
  return false;
}

bool MediaCodecUtils::supportVideoEncoding(const VideoCodecType& codecType) {
  if (codecType == VideoCodecType::kVideoCodecTypeH263
      || codecType == VideoCodecType::kVideoCodecTypeH264
      || codecType == VideoCodecType::kVideoCodecTypeHevc) {
    return true;
  }
  //!< todo checks whether android.media.MediaCodecList supports the VideoCodecType

  return false;
}

std::shared_ptr<MediaFormat>
MediaCodecUtils::convertToMediaFormat(const std::unique_ptr<JniMediaFormat>& jniFormat) {
  bool result;
  int intValue;
  int64_t longValue;
  auto* env = JvmEnv::get().getEnvironment();
  if (env == nullptr) {
    return nullptr;
  }
  std::string strValue;

  // sample mime type
  result = jniFormat->getString(JniMediaFormat::KEY_MIME, &strValue);
  if (!result || strValue.empty()) {
    return nullptr;
  }

  auto mimeType = strValue;
  std::shared_ptr<MediaFormat> format;
  if (MimeTypes::isAudio(mimeType)) {
    int sampleRate;
    int channelCount;
    result = jniFormat->getInteger(JniMediaFormat::KEY_SAMPLE_RATE, &sampleRate);
    if (!result) {
      return nullptr;
    }
    result = jniFormat->getInteger(JniMediaFormat::KEY_CHANNEL_COUNT, &channelCount);
    if (!result) {
      return nullptr;
    }
    format = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
    auto sampleFormat = AudioSampleFormat::kInt16;
    if (jniFormat->getInteger(JniMediaFormat::KEY_PCM_ENCODING, &intValue)) {
      switch (intValue) {
        case JniAudioFormat::ENCODING_PCM_16BIT: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_FLOAT: {
          sampleFormat = AudioSampleFormat::kFloat;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_24BIT_PACKED: {
          sampleFormat = AudioSampleFormat::kInt24;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_32BIT: {
          sampleFormat = AudioSampleFormat::kInt32;
          break;
        }
        default: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
      }
    } else if (jniFormat->getInteger("bits-per-sample", &intValue)) {
      if (intValue == 16) {
        sampleFormat = AudioSampleFormat::kInt16;
      } else if (intValue == 24) {
        sampleFormat = AudioSampleFormat::kInt24;
      } else if (intValue == 32) {
        sampleFormat = AudioSampleFormat::kInt32;
      } else {
        sampleFormat = AudioSampleFormat::kInt16;
      }
    }
    format->setSampleFormat(sampleFormat);
  } else if (MimeTypes::isVideo(mimeType)) {
    int width;
    int height;
    result = jniFormat->getInteger(JniMediaFormat::KEY_WIDTH, &width);
    if (!result) {
      return nullptr;
    }
    result = jniFormat->getInteger(JniMediaFormat::KEY_HEIGHT, &height);
    if (!result) {
      return nullptr;
    }
    format = MediaFormat::CreateVideoFormat(mimeType, width, height);
    if (jniFormat->getInteger(JniMediaFormat::KEY_FRAME_RATE, &intValue)) {
      format->setFrameRate(intValue);
    } else {
      format->setFrameRate(DEFAULT_FRAME_RATE);
    }
    if (jniFormat->getInteger(JniMediaFormat::KEY_I_FRAME_INTERVAL,
      &intValue)) {
      format->setIFrameInterval(intValue);
    }
    if (jniFormat->getInteger(JniMediaFormat::KEY_MAX_B_FRAMES, &intValue)) {
      format->setMaxBFrames(intValue);
    }
    if (jniFormat->getInteger(JniMediaFormat::KEY_ROTATION, &intValue)) {
      format->setRotation(intValue);
    } else {
      format->setRotation(0);
    }
    // color format
    if (jniFormat->getInteger(JniMediaFormat::KEY_COLOR_FORMAT, &intValue)) {
      if (intValue == COLOR_FormatYUV420Planar || intValue == COLOR_FormatYUV420PackedPlanar) {
        format->setPixelFormat(PixelFormat::kFormatI420);
      } else if (intValue == COLOR_FormatYUV420PackedSemiPlanar
                 || intValue == COLOR_FormatYUV420SemiPlanar
                 || intValue == COLOR_QCOM_FormatYUV420SemiPlanar) {
        format->setPixelFormat(PixelFormat::kFormatNV12);
      } else if (intValue == COLOR_FormatRGBAFlexible) {
        format->setPixelFormat(PixelFormat::kFormatRGBA);
      }
    }
  } else {
    // only supports audio and video format parsing
    return nullptr;
  }

  // container mime type
  if (jniFormat->getString("file-format", &strValue)) {
    format->setContainerMimeType(strValue);
  }

  if (jniFormat->getString(JniMediaFormat::KEY_LANGUAGE, &strValue)) {
    format->setLanguage(strValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, &intValue)) {
    format->setMaxInputSize(intValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_BIT_RATE, &intValue)) {
    format->setAverageBitrate(intValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_TRACK_ID, &intValue)) {
    format->setTrackId(intValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_PROFILE, &intValue)) {
    format->setProfile(intValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_LEVEL, &intValue)) {
    format->setLevel(intValue);
  }
  if (jniFormat->getInteger(JniMediaFormat::KEY_FRAME_COUNT, &intValue)) {
    format->setFrameCount(intValue);
  }
  if (jniFormat->getLong(JniMediaFormat::KEY_DURATION, &longValue)) {
    format->setDurationUs(longValue);
  }

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  std::array<std::string, 3> codec_specific_keys = { "csd-0", "csd-1", "csd-2" };
  auto arrayBuffer = std::vector<uint8_t>(DEFAULT_BUFFER_SIZE);
  for (int i = 0; i < codec_specific_keys.size(); ++i) {
    auto byteBuffer = jniFormat->getByteBuffer(codec_specific_keys[i]);
    if (byteBuffer != nullptr) {
      auto bufferSize = byteBuffer->getByteArray(arrayBuffer.data(), DEFAULT_BUFFER_SIZE);
      format->setCodecSpecificData(i, arrayBuffer.data(), bufferSize);
    } else {
      break;
    }
  }

  return format;
}

std::shared_ptr<MediaFormat>
MediaCodecUtils::convertToMediaFormat(AMediaFormat* mediaFormat) {
  bool result;
  int intValue;
  int64_t longValue;
  const char* strValue = nullptr;

  // sample mime type
  result = AMediaFormat_getString(mediaFormat, AMEDIAFORMAT_KEY_MIME, &strValue);
  if (!result || !strValue) {
    return nullptr;
  }

  auto mimeType = strValue;
  std::shared_ptr<MediaFormat> format = nullptr;
  if (MimeTypes::isAudio(mimeType)) {
    int sampleRate;
    int channelCount;
    result = AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate);
    if (!result) {
      return nullptr;
    }
    result = AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channelCount);
    if (!result) {
      return nullptr;
    }
    format = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
    auto sampleFormat = AudioSampleFormat::kInt16;
    if (AMediaFormat_getInt32(mediaFormat, "pcm-encoding", &intValue)) {
      switch (intValue) {
        case JniAudioFormat::ENCODING_PCM_16BIT: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_FLOAT: {
          sampleFormat = AudioSampleFormat::kFloat;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_24BIT_PACKED: {
          sampleFormat = AudioSampleFormat::kInt24;
          break;
        }
        case JniAudioFormat::ENCODING_PCM_32BIT: {
          sampleFormat = AudioSampleFormat::kInt32;
          break;
        }
        default: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
      }
    } else if (AMediaFormat_getInt32(mediaFormat, "bits-per-sample", &intValue)) {
      if (intValue == 16) {
        sampleFormat = AudioSampleFormat::kInt16;
      } else if (intValue == 24) {
        sampleFormat = AudioSampleFormat::kInt24;
      } else if (intValue == 32) {
        sampleFormat = AudioSampleFormat::kInt32;
      } else {
        sampleFormat = AudioSampleFormat::kInt16;
      }
    }
    format->setSampleFormat(sampleFormat);
  } else if (MimeTypes::isVideo(mimeType)) {
    int width;
    int height;
    result = AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, &width);
    if (!result) {
      return nullptr;
    }
    result = AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, &height);
    if (!result) {
      return nullptr;
    }
    format = MediaFormat::CreateVideoFormat(mimeType, width, height);
    if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_FRAME_RATE, &intValue)) {
      format->setFrameRate(intValue);
    } else {
      format->setFrameRate(DEFAULT_FRAME_RATE);
    }
    if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, &intValue)) {
      format->setIFrameInterval(intValue);
    }
    if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_MAX_B_FRAMES, &intValue)) {
      format->setMaxBFrames(intValue);
    }
    if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_ROTATION, &intValue)) {
      format->setRotation(intValue);
    } else {
      format->setRotation(0);
    }
    // color format
    if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, &intValue)) {
      if (intValue == COLOR_FormatYUV420Planar || intValue == COLOR_FormatYUV420PackedPlanar) {
        format->setPixelFormat(PixelFormat::kFormatI420);
      } else if (intValue == COLOR_FormatYUV420SemiPlanar
                 || intValue == COLOR_FormatYUV420PackedSemiPlanar
                 || intValue == COLOR_QCOM_FormatYUV420SemiPlanar) {
        format->setPixelFormat(PixelFormat::kFormatNV12);
      } else if (intValue == COLOR_FormatRGBAFlexible) {
        format->setPixelFormat(PixelFormat::kFormatRGBA);
      }
    }
  } else {
    // only supports audio and video format parsing
    return nullptr;
  }

  // container mime type
  if (AMediaFormat_getString(mediaFormat, "file-format", &strValue)) {
    format->setContainerMimeType(strValue);
  }

  if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, &intValue)) {
    format->setMaxInputSize(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, AMEDIAFORMAT_KEY_BIT_RATE, &intValue)) {
    format->setAverageBitrate(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_TRACK_ID, &intValue)) {
    format->setTrackId(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_PROFILE, &intValue)) {
    format->setProfile(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_LEVEL, &intValue)) {
    format->setLevel(intValue);
  }
  if (AMediaFormat_getInt32(mediaFormat, JniMediaFormat::KEY_FRAME_COUNT, &intValue)) {
    format->setFrameCount(intValue);
  }
  if (AMediaFormat_getInt64(mediaFormat, AMEDIAFORMAT_KEY_DURATION, &longValue)) {
    format->setDurationUs(longValue);
  }

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  std::array<std::string, 3> codec_specific_keys = { "csd-0", "csd-1", "csd-2" };
  void* data = nullptr;
  size_t size = 0;

  for (int i = 0; i < codec_specific_keys.size(); ++i) {
    if (AMediaFormat_getBuffer(mediaFormat, codec_specific_keys[i].c_str(), &data, &size)) {
      if (data && size > 0) {
        format->setCodecSpecificData(i, (const uint8_t*) data, size);
      }
    } else {
      break;
    }
  }

  return format;
}

std::unique_ptr<JniMediaFormat>
MediaCodecUtils::convertToJniMediaFormat(const std::shared_ptr<MediaFormat>& mediaFormat) {
  bool result;
  int intValue;
  int64_t longValue;
  auto env = JvmEnv::get().getEnvironment();
  if (!env) {
    return {};
  }
  if (!mediaFormat->isAudio() && !mediaFormat->isVideo()) {
    return {};
  }
  auto format = JniMediaFormat::create();
  format->setString(JniMediaFormat::KEY_MIME, mediaFormat->getMimeType());
  if (mediaFormat->isAudio()) {
    format->setInteger(JniMediaFormat::KEY_SAMPLE_RATE, mediaFormat->getSampleRate());
    format->setInteger(JniMediaFormat::KEY_CHANNEL_COUNT, mediaFormat->getChannelCount());
    auto audioEncoding = mediaFormat->getSampleFormat();
    int pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
    switch (audioEncoding) {
      case AudioSampleFormat::kFloat: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_FLOAT;
        break;
      }
      case AudioSampleFormat::kInt24: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_24BIT_PACKED;
        break;
      }
      case AudioSampleFormat::kInt32: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_32BIT;
        break;
      }
      default: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
        break;
      }
    }
    format->setInteger(JniMediaFormat::KEY_PCM_ENCODING, pcmEncoding);
  } else if (mediaFormat->isVideo()) {
    format->setInteger(JniMediaFormat::KEY_WIDTH, mediaFormat->getWidth());
    format->setInteger(JniMediaFormat::KEY_HEIGHT, mediaFormat->getHeight());
    int iFrameInterval = mediaFormat->getIFrameInterval();
    if (iFrameInterval <= 0) {
      iFrameInterval = DEFAULT_I_FRAME_INTERVAL;
    }
    format->setInteger(JniMediaFormat::KEY_I_FRAME_INTERVAL, iFrameInterval);
    format->setInteger(JniMediaFormat::KEY_ROTATION, mediaFormat->getRotation());
  }
  if (mediaFormat->getMaxInputSize() >= 0) {
    format->setInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, mediaFormat->getMaxInputSize());
  }
  if (mediaFormat->getTrackId() != Definitions::kInvalidValue) {
    format->setInteger(JniMediaFormat::KEY_TRACK_ID, mediaFormat->getTrackId());
  }
  if (mediaFormat->getProfile() != Definitions::kInvalidValue) {
    format->setInteger(JniMediaFormat::KEY_PROFILE, mediaFormat->getProfile());
  }
  if (mediaFormat->getLevel() != Definitions::kInvalidValue) {
    format->setInteger(JniMediaFormat::KEY_LEVEL, mediaFormat->getLevel());
  }
  if (mediaFormat->getFrameCount() != Definitions::kInvalidValue) {
    format->setInteger(JniMediaFormat::KEY_FRAME_COUNT,
      mediaFormat->getFrameCount());
  }
  if (mediaFormat->getDurationUs() != Definitions::kTimeUnset) {
    format->setLong(JniMediaFormat::KEY_DURATION, mediaFormat->getDurationUs());
  }

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  std::array<std::string, 3> codec_specific_keys = { "csd-0", "csd-1", "csd-2" };
  auto const& csdList = mediaFormat->csdList();
  auto csdSize = std::min(codec_specific_keys.size(), csdList.size());
  for (auto i = 0; i < csdSize; ++i) {
    if (csdList[i].empty()) {
      break;
    } else {
      format->setByteBuffer(codec_specific_keys[i], (void*) csdList[i].data(), csdList[i].size());
    }
  }

  return format;
}

AMediaFormat*
MediaCodecUtils::convertToNdkMediaFormat(const std::shared_ptr<MediaFormat>& mediaFormat) {
  if (!mediaFormat->isAudio() && !mediaFormat->isVideo()) {
    return nullptr;
  }
  auto format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mediaFormat->getMimeType().c_str());
  if (mediaFormat->isAudio()) {
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, mediaFormat->getSampleRate());
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, mediaFormat->getChannelCount());
    auto audioEncoding = mediaFormat->getSampleFormat();
    int pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
    switch (audioEncoding) {
      case AudioSampleFormat::kFloat: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_FLOAT;
        break;
      }
      case AudioSampleFormat::kInt24: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_24BIT_PACKED;
        break;
      }
      case AudioSampleFormat::kInt32: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_32BIT;
        break;
      }
      default: {
        pcmEncoding = JniAudioFormat::ENCODING_PCM_16BIT;
        break;
      }
    }
    AMediaFormat_setInt32(format, "pcm-encoding", pcmEncoding);
  } else if (mediaFormat->isVideo()) {
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, mediaFormat->getWidth());
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, mediaFormat->getHeight());
    int iFrameInterval = mediaFormat->getIFrameInterval();
    if (iFrameInterval <= 0) {
      iFrameInterval = DEFAULT_I_FRAME_INTERVAL;
    }
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, iFrameInterval);
    AMediaFormat_setInt32(format, "rotation-degrees", mediaFormat->getRotation());
  }
  if (mediaFormat->getMaxInputSize() >= 0) {
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, mediaFormat->getMaxInputSize());
  }
  if (mediaFormat->getTrackId() != Definitions::kInvalidValue) {
    AMediaFormat_setInt32(format, "track-id", mediaFormat->getTrackId());
  }
  if (mediaFormat->getProfile() != Definitions::kInvalidValue) {
    AMediaFormat_setInt32(format, "profile", mediaFormat->getProfile());
  }
  if (mediaFormat->getLevel() != Definitions::kInvalidValue) {
    AMediaFormat_setInt32(format, "level", mediaFormat->getLevel());
  }
  if (mediaFormat->getFrameCount() != Definitions::kInvalidValue) {
    AMediaFormat_setInt32(format, "frame-count", mediaFormat->getFrameCount());
  }
  if (mediaFormat->getDurationUs() != Definitions::kTimeUnset) {
    AMediaFormat_setInt64(format, AMEDIAFORMAT_KEY_DURATION, mediaFormat->getDurationUs());
  }

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  std::array<std::string, 3> codec_specific_keys = { "csd-0", "csd-1", "csd-2" };
  auto const& csdList = mediaFormat->csdList();
  auto csdSize = std::min(codec_specific_keys.size(), csdList.size());
  for (auto i = 0; i < csdSize; ++i) {
    if (csdList[i].empty()) {
      break;
    }
    AMediaFormat_setBuffer(
      format, codec_specific_keys[i].c_str(), csdList[i].data(), csdList[i].size());
  }

  return format;
}

void MediaCodecUtils::NV12toYUV420Planar(const uint8_t* input, const int offset, uint8_t* output,
  const int width, const int height) {
  const int frameSize = width * height;
  const int qFrameSize = frameSize / 4;

  memmove(output, input + offset, frameSize); // Y
  int i = 0;
  for (i = 0; i < qFrameSize; i++) {
    output[frameSize + i] = input[offset + frameSize + i * 2]; // U
    output[frameSize + qFrameSize + i] =
      input[offset + frameSize + i * 2 + 1]; // V
  }
}

void MediaCodecUtils::NV21toYUV420Planar(const uint8_t* input, const int offset, uint8_t* output,
  const int width, const int height) {
  const int frameSize = width * height;
  const int qFrameSize = frameSize / 4;

  memmove(output, input + offset, frameSize); // Y
  int i = 0;
  for (i = 0; i < qFrameSize; i++) {
    output[frameSize + i] = input[offset + frameSize + i * 2 + 1]; // U
    output[frameSize + qFrameSize + i] = input[offset + frameSize + i * 2]; // V
  }
}

void MediaCodecUtils::I420toYUV420SemiPlanar(const uint8_t* input, const int offset, uint8_t* output,
  const int width, const int height) {
  const int frameSize = width * height;
  const int qFrameSize = frameSize / 4;

  memmove(output, input + offset, frameSize); // Y
  int i = 0;
  for (i = 0; i < qFrameSize; i++) {
    output[frameSize + i * 2] = input[offset + frameSize + i]; // Cb (U)
    output[frameSize + i * 2 + 1] =
      input[offset + frameSize + i + qFrameSize]; // Cr (V)
  }
}

void MediaCodecUtils::I420toNV21(const uint8_t* input, const int offset, uint8_t* output,
  const int width, const int height) {
  const int frameSize = width * height;
  const int qFrameSize = frameSize / 4;

  memmove(output, input + offset, frameSize); // Y
  int i = 0;
  for (i = 0; i < qFrameSize; i++) {
    output[frameSize + i * 2 + 1] = input[offset + frameSize + i]; // Cb (U)
    output[frameSize + i * 2] =
      input[offset + frameSize + i + qFrameSize]; // Cr (V)
  }
}

void MediaCodecUtils::NV12toNV21(const uint8_t* input, const int offset, uint8_t* output,
  const int width, const int height) {
  const int frameSize = width * height;
  const int qFrameSize = frameSize / 2;

  memmove(output, input + offset, frameSize); // Y
  int i = 0;
  for (i = 0; i + 1 < qFrameSize; i += 2) {
    output[frameSize + i] = input[offset + frameSize + i + 1]; // U
    output[frameSize + i + 1] = input[offset + frameSize + i]; // V
  }
}

} // namespace coral
