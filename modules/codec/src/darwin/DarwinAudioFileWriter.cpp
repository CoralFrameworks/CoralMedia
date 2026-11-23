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

#include "DarwinAudioFileWriter.h"

#include <media/Definitions.h>
#include <media/Types.h>

#include <minilog/log.h>

#include <algorithm>
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

std::unique_ptr<AudioFileWriter> DarwinAudioFileWriter::sniff(const std::string& outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return nullptr;
  }
  if (!isSupportedFileType(fileType)) {
    return nullptr;
  }
  if (format->getSampleRate() <= 0 || format->getChannelCount() <= 0) {
    return nullptr;
  }
  if (!isSupportedSampleFormat(format->getSampleFormat())) {
    MINILOG_DEBUG("the audio file writer does not support the pcm format: {}",
      convertToText(format->getSampleFormat()));
    return nullptr;
  }

  return std::unique_ptr<AudioFileWriter>(new DarwinAudioFileWriter(outputUrl, fileType, format));
}

DarwinAudioFileWriter::DarwinAudioFileWriter(std::string outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) noexcept
  : AudioFileWriter(format->getSampleFormat(), static_cast<uint64_t>(format->getSampleRate()),
      static_cast<uint32_t>(format->getChannelCount())),
    mOutputUrl(std::move(outputUrl)),
    mFileType(fileType),
    mInputFormat(format) {
}

DarwinAudioFileWriter::~DarwinAudioFileWriter() {
  close();
}

bool DarwinAudioFileWriter::isSupportedFileType(const MediaFileType& fileType) {
  return fileType == MediaFileType::kMediaFileTypeMpeg4AAC ||
         fileType == MediaFileType::kMediaFileTypeFlac ||
         fileType == MediaFileType::kMediaFileTypeWav;
}

bool DarwinAudioFileWriter::convertToFileType(const MediaFileType& fileType,
  AudioFileTypeID& fileTypeId) {
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC: {
      fileTypeId = kAudioFileM4AType;
      return true;
    }
    case MediaFileType::kMediaFileTypeFlac: {
      fileTypeId = kAudioFileFLACType;
      return true;
    }
    case MediaFileType::kMediaFileTypeWav: {
      fileTypeId = kAudioFileWAVEType;
      return true;
    }
    default: {
      return false;
    }
  }
}

bool DarwinAudioFileWriter::isSupportedSampleFormat(const AudioSampleFormat sampleFormat) {
  return sampleFormat == AudioSampleFormat::kInt16 ||
         sampleFormat == AudioSampleFormat::kInt24 ||
         sampleFormat == AudioSampleFormat::kInt32 ||
         sampleFormat == AudioSampleFormat::kFloat;
}

void DarwinAudioFileWriter::createFileFormat(AudioStreamBasicDescription& format) const {
  const auto sampleRate = static_cast<int>(getSampleRate());
  const auto channelCount = static_cast<int>(getChannelCount());
  switch (mFileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC: {
      format = darwin::makeCompressedFormat(AudioFormatType::kAudioFormatTypeMpeg4AAC, sampleRate,
        channelCount);
      break;
    }
    case MediaFileType::kMediaFileTypeFlac: {
      format = darwin::makeCompressedFormat(AudioFormatType::kAudioFormatTypeFlac, sampleRate,
        channelCount);
      break;
    }
    case MediaFileType::kMediaFileTypeWav:
    default: {
      format = darwin::makePcmFormat(sampleRate, channelCount, getSampleFormat());
      break;
    }
  }
}

bool DarwinAudioFileWriter::open(Error** outError) {
  if (mOpened) {
    return true;
  }
  if (mExtFile != nullptr) {
    close();
  }
  if (mOutputUrl.empty()) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file writer",
      "The output url is empty");
    return false;
  }
  if (mInputFormat == nullptr) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file writer",
      "The format is null");
    return false;
  }

  AudioFileTypeID fileTypeId = 0;
  if (!convertToFileType(mFileType, fileTypeId)) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file writer",
      "Unsupported file type");
    return false;
  }

  const auto fileUrl = createFileUrl(mOutputUrl);
  if (fileUrl == nullptr) {
    handleError(outError, ErrCode::kInvalidArgument, "Failed to open the audio file writer",
      "The output url is invalid");
    return false;
  }

  AudioStreamBasicDescription fileFormat{};
  createFileFormat(fileFormat);
  const auto status = ExtAudioFileCreateWithURL(fileUrl, fileTypeId, &fileFormat, nullptr,
    kAudioFileFlags_EraseFile, &mExtFile);
  CFRelease(fileUrl);
  if (status != noErr || mExtFile == nullptr) {
    mExtFile = nullptr;
    handleError(outError, "Failed to open the audio file writer",
      darwin::osStatusToString(status).c_str());
    return false;
  }

  const auto clientFormat = darwin::makePcmFormat(static_cast<int>(getSampleRate()),
    static_cast<int>(getChannelCount()), getSampleFormat());
  const auto setStatus = ExtAudioFileSetProperty(mExtFile, kExtAudioFileProperty_ClientDataFormat,
    sizeof(clientFormat), &clientFormat);
  if (setStatus != noErr) {
    darwin::logStatus(setStatus, "failed to set the audio file writer client format", mOutputUrl);
    handleError(outError, "Failed to open the audio file writer",
      darwin::osStatusToString(setStatus).c_str());
    close();
    return false;
  }
  mBytesPerFrame = darwin::bytesPerFrameOf(clientFormat);
  if (mBytesPerFrame <= 0) {
    handleError(outError, "Failed to open the audio file writer", "The output format is invalid");
    close();
    return false;
  }

  // The m4a container stores aac, whose encoder ExtAudioFile created together with the client
  // format. Its bit rate has to be set on that converter.
  if (mFileType == MediaFileType::kMediaFileTypeMpeg4AAC) {
    applyEncodeBitrate();
  }

  mFramePosition = 0;
  mOpened = true;
  MINILOG_DEBUG("opened the audio file: {}, sampleRate: {}, channelCount: {}", mOutputUrl,
    getSampleRate(), getChannelCount());
  return true;
}

void DarwinAudioFileWriter::applyEncodeBitrate() {
  AudioConverterRef converter = nullptr;
  UInt32 size = sizeof(converter);
  const auto status = ExtAudioFileGetProperty(mExtFile, kExtAudioFileProperty_AudioConverter, &size,
    &converter);
  if (status != noErr || converter == nullptr) {
    MINILOG_WARN("the audio file writer has no aac converter: {}", darwin::osStatusToString(status));
    return;
  }

  auto bitrate = mInputFormat->getAverageBitrate();
  if (bitrate <= 0) {
    bitrate = mInputFormat->getBitrate();
  }
  if (bitrate <= 0) {
    bitrate = Definitions::DEFAULT_AUDIO_BITRATE;
  }

  auto value = static_cast<UInt32>(bitrate);
  const auto setStatus = AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate,
    sizeof(value), &value);
  if (setStatus != noErr) {
    MINILOG_WARN("the aac encoder ignored the bit rate {}: {}", bitrate,
      darwin::osStatusToString(setStatus));
  }
}

int64_t DarwinAudioFileWriter::writeFromPCMBuffer(void* inputBuffer, const size_t numFrames,
  Error** outError) {
  if (!mOpened || mExtFile == nullptr) {
    handleError(outError, ErrCode::kNotInit, "Failed to write the audio frames",
      "The writer is not open");
    return Definitions::kInvalidValue;
  }
  if (inputBuffer == nullptr || numFrames == 0) {
    return 0;
  }
  if (mBytesPerFrame <= 0) {
    handleError(outError, "Failed to write the audio frames", "The output format is invalid");
    return Definitions::kInvalidValue;
  }

  AudioBufferList bufferList{};
  bufferList.mNumberBuffers = 1;
  bufferList.mBuffers[0].mNumberChannels = static_cast<UInt32>(getChannelCount());
  bufferList.mBuffers[0].mDataByteSize = static_cast<UInt32>(numFrames) *
                                         static_cast<UInt32>(mBytesPerFrame);
  bufferList.mBuffers[0].mData = inputBuffer;

  const auto status = ExtAudioFileWrite(mExtFile, static_cast<UInt32>(numFrames), &bufferList);
  if (status != noErr) {
    darwin::logStatus(status, "failed to write the audio frames", mOutputUrl);
    handleError(outError, "Failed to write the audio frames",
      darwin::osStatusToString(status).c_str());
    return Definitions::kInvalidValue;
  }

  mFramePosition += static_cast<int64_t>(numFrames);
  return static_cast<int64_t>(numFrames);
}

void DarwinAudioFileWriter::close() {
  if (mExtFile != nullptr) {
    // Disposing the file finalizes the container, including the movie header of an m4a file.
    const auto status = ExtAudioFileDispose(mExtFile);
    if (status != noErr) {
      darwin::logStatus(status, "failed to close the audio file", mOutputUrl);
    }
    mExtFile = nullptr;
  }
  mOpened = false;
}

} // namespace coral
