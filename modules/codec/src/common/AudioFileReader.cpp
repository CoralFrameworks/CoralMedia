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

#include <codec/AudioFileReader.h>

#include <minilog/log.h>

#include "common/WaveFileReader.h"

#if defined(ANDROID) || defined(__ANDROID__)
#include "android/NdkAudioFileReader.h"
#endif

#if defined(WINDOWS) || defined(WIN32)
#include "win/WmfAudioFileReader.h"
#endif

namespace coral {

AudioFileReader::~AudioFileReader() = default;

std::unique_ptr<AudioFileReader>
AudioFileReader::CreateWithUrl(const std::string& inputUrl) {
  std::unique_ptr<AudioFileReader> reader = nullptr;

  // sniff whether is a wav file
  reader = WaveFileReader::sniff(inputUrl);

  // use platform reader prefer
#if defined(ANDROID) || defined(__ANDROID__)
  if (reader == nullptr) {
    reader = NdkAudioFileReader::sniff(inputUrl);
  }
#endif // __ANDROID__

#if defined(WINDOWS) || defined(WIN32)
  if (reader == nullptr) {
    reader = WmfAudioFileReader::sniff(inputUrl);
  }
#endif

  return reader;
}

void AudioFileReader::logError(const ErrCode code, const char* errorMsg, const char* description) {
  MINILOG_ERROR("ErrCode: {}, msg: {}, description: {}", std::to_string(code), errorMsg, description);
}

} // namespace coral
