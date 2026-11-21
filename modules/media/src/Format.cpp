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

#include <media/Format.h>
#include <media/Utilities.h>

#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace coral {

std::shared_ptr<MediaFormat>
MediaFormat::CreateAudioFormat(const std::string& mime, const int sampleRate, const int channelCount) {
  auto format = std::shared_ptr<MediaFormat>(new MediaFormat());
  format->mMimeType = mime;
  format->mSampleRate = sampleRate;
  format->mChannelCount = channelCount;
  format->mAudioFormatType = MediaUtils::convertToAudioFormatType(mime);
  return format;
}

std::shared_ptr<MediaFormat>
MediaFormat::CreateVideoFormat(const std::string& mime, int width, int height) {
  auto format = std::shared_ptr<MediaFormat>(new MediaFormat());
  format->mMimeType = mime;
  format->mWidth = width;
  format->mHeight = height;
  format->mVideoCodecType = MediaUtils::convertToVideoCodecType(mime);
  return format;
}

std::shared_ptr<MediaFormat>
MediaFormat::CreateSubtitleFormat(const std::string& mime) {
  auto format = std::shared_ptr<MediaFormat>(new MediaFormat());
  format->mMimeType = mime;
  return format;
}

MediaFormat::MediaFormat()
  : mTrackId(Definitions::kInvalidValue),
    mDurationUs(Definitions::kTimeUnset),
    mFrameCount(NO_VALUE),
    mSelectionFlags(MediaSelectionFlags::kMediaSelectionDefault),
    mAverageBitrate(NO_VALUE),
    mPeakBitrate(NO_VALUE),
    mBitrate(NO_VALUE),
    mMaxInputSize(NO_VALUE),
    mProfile(NO_VALUE),
    mLevel(NO_VALUE),
    mChannelCount(NO_VALUE),
    mSampleRate(NO_VALUE),
    mAudioFormatType(AudioFormatType::kAudioFormatTypeInvalid),
    mSampleFormat(AudioSampleFormat::kInvalid),
    mEncoderDelay(0),
    mEncoderPadding(0),
    mWidth(NO_VALUE),
    mHeight(NO_VALUE),
    mFrameRate(NO_VALUE),
    mRotationDegrees(NO_VALUE),
    mVideoCodecType(VideoCodecType::kVideoCodecTypeInvalid),
    mPixelFormat(PixelFormat::kFormatInvalid),
    mIFrameInterval(NO_VALUE),
    mMaxBFrames(0),
    mSubsampleOffsetUs(0),
    mCueReplacementBehavior(CueReplacementBehavior::kMerge) {
}

void MediaFormat::setAverageBitrate(const int bitrate) {
  mAverageBitrate = bitrate;
  adjustBitRate();
}

void MediaFormat::setPeakBitrate(const int bitrate) {
  mPeakBitrate = bitrate;
  adjustBitRate();
}

void MediaFormat::adjustBitRate() {
  if (mPeakBitrate != NO_VALUE) {
    mBitrate = mPeakBitrate;
  } else if (mAverageBitrate != NO_VALUE) {
    mBitrate = mAverageBitrate;
  }
}

void MediaFormat::setCodecSpecificData(int index, const uint8_t* data, size_t size) {
  if (index >= MAX_CSD_LENGTH) {
    return;
  }
  if (mCsdList.size() <= static_cast<size_t>(index)) {
    mCsdList.resize(static_cast<size_t>(index) + 1);
  }
  auto csdBuffer = std::vector<uint8_t>(size);
  for (auto i = 0; i < size; ++i) {
    csdBuffer[i] = data[i];
  }
  mCsdList[index] = csdBuffer;
}

void MediaFormat::setCodecSpecificData(int index, const std::vector<uint8_t>& data) {
  setCodecSpecificData(index, data.data(), data.size());
}

void MediaFormat::setCodecSpecificData(
  const std::vector<std::vector<uint8_t>>& data) {
  for (auto i = 0; i < std::min((int) data.size(), MAX_CSD_LENGTH); ++i) {
    setCodecSpecificData(i, data[i]);
  }
}

std::shared_ptr<MediaFormat> MediaFormat::buildUpon() const {
  auto format = std::shared_ptr<MediaFormat>(new MediaFormat());
  format->id = id;
  format->label = label;
  format->mTrackId = mTrackId;
  format->mDerivedTrack = mDerivedTrack;
  format->mDurationUs = mDurationUs;
  format->mFrameCount = mFrameCount;
  format->mLanguage = mLanguage;
  format->mSelectionFlags = mSelectionFlags;
  format->mRoleFlags = mRoleFlags;
  format->mCaptionServiceNumber = mCaptionServiceNumber;
  format->mAverageBitrate = mAverageBitrate;
  format->mPeakBitrate = mPeakBitrate;
  format->mBitrate = mBitrate;
  format->codecs = codecs;
  format->mMetadata = mMetadata;
  format->mContainerMimeType = mContainerMimeType;
  format->mMimeType = mMimeType;
  format->mMaxInputSize = mMaxInputSize;
  format->mProfile = mProfile;
  format->mLevel = mLevel;
  format->mCsdList = mCsdList;
  format->mChannelCount = mChannelCount;
  format->mSampleRate = mSampleRate;
  format->mAudioFormatType = mAudioFormatType;
  format->mSampleFormat = mSampleFormat;
  format->mEncoderDelay = mEncoderDelay;
  format->mEncoderPadding = mEncoderPadding;
  format->mWidth = mWidth;
  format->mHeight = mHeight;
  format->mFrameRate = mFrameRate;
  format->mRotationDegrees = mRotationDegrees;
  format->mColorInfo = mColorInfo;
  format->mVideoCodecType = mVideoCodecType;
  format->mPixelFormat = mPixelFormat;
  format->mIFrameInterval = mIFrameInterval;
  format->mMaxBFrames = mMaxBFrames;
  format->mMaxSubLayers = mMaxSubLayers;
  format->mSubsampleOffsetUs = mSubsampleOffsetUs;
  format->mCueReplacementBehavior = mCueReplacementBehavior;
  return format;
}

std::string MediaFormat::toString() const {
  std::string str;
  str.append("{\n");
  if (mMimeType.empty()) {
    str.append("\n}");
    return str;
  }
  str.append("mimeType=").append(mMimeType);
  if (mTrackId != Definitions::kInvalidValue) {
    str.append(",\ntrackId=").append(std::to_string(mTrackId));
  }
  if (!mLanguage.empty()) {
    str.append(",\nlanguage=").append(mLanguage);
  }
  if (isSubtitle()) {
    str.append(",\ncue-replacement-behavior=")
       .append(mCueReplacementBehavior == CueReplacementBehavior::kReplace ? "replace" : "merge");
    if (mSubsampleOffsetUs != OFFSET_SAMPLE_RELATIVE) {
      str.append(",\nsubsample-offset-us=").append(std::to_string(mSubsampleOffsetUs));
    }
  }
  if ((mMimeType.find("audio/") != std::string::npos)) {
    str.append(",\nsampleRate=")
       .append(std::to_string(mSampleRate))
       .append(",\nchannels=")
       .append(std::to_string(mChannelCount))
       .append(",\naudioSampleFormat=")
       .append(convertToText(mSampleFormat));
  }
  if ((mMimeType.find("video/") != std::string::npos)) {
    str.append(",\nwidth=")
       .append(std::to_string(mWidth))
       .append(",\nheight=")
       .append(std::to_string(mHeight))
       .append(",\nrotate-degrees=")
       .append(std::to_string(mRotationDegrees))
       .append(",\npixelformat=")
       .append(convertToText((mPixelFormat)));
    if (mFrameRate != NO_VALUE) {
      str.append(",\nframe-rate=").append(std::to_string(mFrameRate));
    }
    if (mIFrameInterval != NO_VALUE) {
      str.append(",\ni-frame-interval=")
         .append(std::to_string(mIFrameInterval));
    }
    if (mMaxBFrames != NO_VALUE) {
      str.append(",\nb-frames=").append(std::to_string(mMaxBFrames));
    }
  }
  if (mProfile != NO_VALUE) {
    str.append(",\nprofile=").append(std::to_string(mProfile));
  }
  if (mLevel != NO_VALUE) {
    str.append(",\nlevel=").append(std::to_string(mLevel));
  }
  if (mBitrate != NO_VALUE) {
    str.append(",\nbitrate=").append(std::to_string(mBitrate));
  }
  if (mMaxInputSize != NO_VALUE) {
    str.append(",\nmax-input-sizes=").append(std::to_string(mMaxInputSize));
  }

  if (!mCsdList.empty()) {
    std::array<std::string, MediaFormat::MAX_CSD_LENGTH> codec_specific_keys = {
      ",\ncsd-0=", ",\ncsd-1=", ",\ncsd-2="
    };
    auto csdSize = std::min(MediaFormat::MAX_CSD_LENGTH, (int) mCsdList.size());
    for (auto i = 0; i < csdSize; ++i) {
      auto const& csd = mCsdList[i];
      if (csd.empty()) {
        break;
      } else {
        std::stringstream csdStr;
        for (uint8_t byte: csd) {
          csdStr << std::hex << static_cast<int>(byte) << " ";
        }
        str.append(codec_specific_keys[i]).append(": { ").append(csdStr.str()).append("}");
      }
    }
  }
  str.append("\n}");
  return str;
}

} // namespace coral
