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

#include "DarwinAudioFileReader.h"

#include "DarwinMediaUtils.h"

#include <media/Definitions.h>

#include <minilog/log.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace coral {

namespace {

/**
 * Creates the file url of a coral url.
 *
 * The coral urls are either plain file system paths or `file://` urls.
 */
CFURLRef createFileUrl(const std::string& url) {
  if (url.empty()) {
    return nullptr;
  }
  auto* string = CFStringCreateWithCString(kCFAllocatorDefault, url.c_str(), kCFStringEncodingUTF8);
  if (string == nullptr) {
    return nullptr;
  }

  CFURLRef fileUrl = nullptr;
  if (url.rfind("file://", 0) == 0) {
    fileUrl = CFURLCreateWithString(kCFAllocatorDefault, string, nullptr);
  } else {
    fileUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, string, kCFURLPOSIXPathStyle,
      false);
  }
  CFRelease(string);
  return fileUrl;
}

} // namespace anonymous

std::unique_ptr<DarwinAudioFileReader> DarwinAudioFileReader::sniff(const std::string& inputUrl) {
  auto reader = std::make_unique<DarwinAudioFileReader>(inputUrl);
  if (!reader->open(nullptr)) {
    return nullptr;
  }
  if (reader->getSampleRate() <= 0 || reader->getChannelCount() <= 0) {
    reader->close();
    return nullptr;
  }
  return reader;
}

DarwinAudioFileReader::DarwinAudioFileReader(std::string inputUrl)
  : mInputUrl(std::move(inputUrl)) {
}

DarwinAudioFileReader::~DarwinAudioFileReader() {
  close();
}

bool DarwinAudioFileReader::open(Error** outError) {
  if (mOpened) {
    return true;
  }
  if (mExtFile != nullptr) {
    close();
  }
  if (mInputUrl.empty()) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file reader",
      "The input url is empty");
    return false;
  }

  const auto fileUrl = createFileUrl(mInputUrl);
  if (fileUrl == nullptr) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file reader",
      "The input url is invalid");
    return false;
  }

  const auto status = ExtAudioFileOpenURL(fileUrl, &mExtFile);
  CFRelease(fileUrl);
  if (status != noErr || mExtFile == nullptr) {
    mExtFile = nullptr;
    handleError(outError, "Failed to open the audio file reader",
      darwin::osStatusToString(status).c_str());
    return false;
  }

  if (!readFormatInfo(outError)) {
    close();
    return false;
  }

  // The platform decodes the stored audio to interleaved 16 bit signed pcm while it is read.
  const auto clientFormat = darwin::makePcmFormat(static_cast<int>(mSampleRate), mChannelCount,
    AudioSampleFormat::kInt16);
  const auto setStatus = ExtAudioFileSetProperty(mExtFile, kExtAudioFileProperty_ClientDataFormat,
    sizeof(clientFormat), &clientFormat);
  if (setStatus != noErr) {
    darwin::logStatus(setStatus, "failed to set the audio file reader client format", mInputUrl);
    handleError(outError, "Failed to open the audio file reader",
      darwin::osStatusToString(setStatus).c_str());
    close();
    return false;
  }

  mSampleFormat = AudioSampleFormat::kInt16;
  mBytesPerFrame = darwin::bytesPerFrameOf(clientFormat);
  if (mBytesPerFrame <= 0) {
    handleError(outError, "Failed to open the audio file reader", "The output format is invalid");
    close();
    return false;
  }

  mFramePosition = 0;
  mOpened = true;
  MINILOG_DEBUG("opened the audio file: {}, sampleRate: {}, channelCount: {}, frames: {}", mInputUrl,
    mSampleRate, mChannelCount, mSampleLength);
  return true;
}

bool DarwinAudioFileReader::readFormatInfo(Error** outError) {
  AudioStreamBasicDescription fileFormat{};
  UInt32 size = sizeof(fileFormat);
  auto status = ExtAudioFileGetProperty(mExtFile, kExtAudioFileProperty_FileDataFormat, &size,
    &fileFormat);
  if (status != noErr) {
    darwin::logStatus(status, "failed to read the audio file format", mInputUrl);
    handleError(outError, "Failed to open the audio file reader",
      darwin::osStatusToString(status).c_str());
    return false;
  }
  if (fileFormat.mSampleRate <= 0 || fileFormat.mChannelsPerFrame == 0) {
    handleError(outError, "Failed to open the audio file reader", "The file format is invalid");
    return false;
  }

  mSampleRate = static_cast<int64_t>(fileFormat.mSampleRate);
  mChannelCount = static_cast<int32_t>(fileFormat.mChannelsPerFrame);

  // The length of the file is expressed in sample frames, not in packets.
  SInt64 sampleLength = 0;
  size = sizeof(sampleLength);
  status = ExtAudioFileGetProperty(mExtFile, kExtAudioFileProperty_FileLengthFrames, &size,
    &sampleLength);
  if (status == noErr && sampleLength >= 0) {
    mSampleLength = static_cast<int64_t>(sampleLength);
  } else {
    mSampleLength = Definitions::kInvalidValue;
  }
  return true;
}

int64_t DarwinAudioFileReader::readIntoBuffer(uint8_t* outBuffer, const int64_t bufferCapacity,
  Error** outError) {
  if (!mOpened || mExtFile == nullptr) {
    handleError(outError, ErrCode::kNotInit, "Failed to read the audio frames",
      "The reader is not open");
    return Definitions::kInvalidValue;
  }
  if (outBuffer == nullptr || bufferCapacity <= 0) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to read the audio frames",
      "The buffer is invalid");
    return Definitions::kInvalidValue;
  }
  if (mBytesPerFrame <= 0 || mChannelCount <= 0) {
    handleError(outError, "Failed to read the audio frames", "The output format is invalid");
    return Definitions::kInvalidValue;
  }

  const auto capacityFrames = bufferCapacity / mBytesPerFrame;
  if (capacityFrames <= 0) {
    return 0;
  }
  const auto framesToRead = static_cast<UInt32>(
    std::min<int64_t>(capacityFrames, std::numeric_limits<UInt32>::max()));

  AudioBufferList bufferList{};
  bufferList.mNumberBuffers = 1;
  bufferList.mBuffers[0].mNumberChannels = static_cast<UInt32>(mChannelCount);
  bufferList.mBuffers[0].mDataByteSize = framesToRead * static_cast<UInt32>(mBytesPerFrame);
  bufferList.mBuffers[0].mData = outBuffer;

  auto frames = framesToRead;
  const auto status = ExtAudioFileRead(mExtFile, &frames, &bufferList);
  if (status != noErr) {
    darwin::logStatus(status, "failed to read the audio frames", mInputUrl);
    handleError(outError, "Failed to read the audio frames",
      darwin::osStatusToString(status).c_str());
    return Definitions::kInvalidValue;
  }

  mFramePosition += static_cast<int64_t>(frames);
  return static_cast<int64_t>(frames);
}

void DarwinAudioFileReader::seekTo(const int64_t framePosition) {
  if (!mOpened || mExtFile == nullptr) {
    return;
  }

  auto target = std::max<int64_t>(framePosition, 0);
  if (mSampleLength > 0) {
    target = std::min(target, mSampleLength);
  }

  const auto status = ExtAudioFileSeek(mExtFile, target);
  if (status != noErr) {
    MINILOG_ERROR("failed to seek the audio file to the frame {}: {}", target,
      darwin::osStatusToString(status));
    return;
  }
  mFramePosition = target;
}

void DarwinAudioFileReader::close() {
  if (mExtFile != nullptr) {
    ExtAudioFileDispose(mExtFile);
    mExtFile = nullptr;
  }
  mFramePosition = 0;
  mOpened = false;
}

} // namespace coral
