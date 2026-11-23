/*
 * Copyright (C) 2024 The Coral Project
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

#include <codec/AudioFileWriter.h>

#include <minilog/log.h>

#include "AudioMuxFileWriter.h"
#include "WaveFileWriter.h"

#if defined(WINDOWS) || defined(WIN32)
#include "win/WmfAudioFileWriter.h"
#endif

#if defined(__APPLE__)
#include "darwin/DarwinAudioFileWriter.h"
#endif

namespace coral {

AudioFileWriter::~AudioFileWriter() noexcept = default;

std::unique_ptr<AudioFileWriter>
AudioFileWriter::CreateWithUrl(const std::string& url,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) {

  const auto AudioFormatType = format->getAudioFormatType();
  if (AudioFormatType == AudioFormatType::kAudioFormatTypeInvalid) {
    return nullptr;
  }

  // use WaveFileWriter
  if (fileType == MediaFileType::kMediaFileTypeWav
      && AudioFormatType == AudioFormatType::kAudioFormatTypeLinearPCM) {
    if (auto writer = WaveFileWriter::sniff(url, format)) {
      return writer;
    }
  }

#if defined(WINDOWS) || defined(WIN32)
  // the media foundation sink writer handles the containers and the compressed audio codecs
  // which are provided by the Windows platform.
  if (auto writer = WmfAudioFileWriter::sniff(url, fileType, format)) {
    return writer;
  }
#endif

#if defined(__APPLE__)
  // the ExtAudioFile based writer handles the containers and the compressed audio codecs which
  // are provided by the platform.
  if (auto writer = DarwinAudioFileWriter::sniff(url, fileType, format)) {
    return writer;
  }
#endif

  return AudioMuxFileWriter::sniff(url, fileType, format);
}

void AudioFileWriter::handleError(Error** outError, ErrCode errorCode, const char* errorMsg, const char* description) {
  if (outError != nullptr) {
    if (*outError == nullptr) {
      *outError = new Error();
    }
    (*outError)->code = errorCode;
    (*outError)->failureReason = errorMsg;
#ifndef NDEBUG
    logError(errorCode, errorMsg, description);
#endif
  } else {
    logError(errorCode, errorMsg, description);
  }
}

void AudioFileWriter::logError(const ErrCode code, const char* errorMsg, const char* description) {
  MINILOG_ERROR("error: {0}, msg:{1}, description: {2}", std::to_string(code), errorMsg,
    (description == nullptr ? "null" : description));
}

} // namespace coral
