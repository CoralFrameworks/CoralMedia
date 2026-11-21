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

#include "WaveFileReader.h"

#include <minilog/log.h>

#include <utility>

namespace coral {

std::unique_ptr<WaveFileReader>
WaveFileReader::sniff(const std::string& inputUrl) {
  if (!isWaveFile(inputUrl)) {
    return nullptr;
  }
  MINILOG_DEBUG("Use AudioWaveFileReader for file: {}", inputUrl);
  return std::make_unique<WaveFileReader>(inputUrl);
}

WaveFileReader::WaveFileReader(std::string inputUrl) noexcept
  : mInputUrl(std::move(inputUrl)),
    mSampleLength(0),
    mFrameLength(0),
    mFileLength(0),
    mDataLength(0),
    mChannels(0),
    mSampleRate(0),
    mBitsPerSample(0),
    mDataOffset(0),
    mSampleFormat(AudioSampleFormat::kInt16) {
}

WaveFileReader::~WaveFileReader() {
  close();
}

bool WaveFileReader::open(Error** outError) {
  WaveRIFF riff;
  WaveFormat format;
  WaveData data;
  int userDataSize;

  mFile.reset(fopen(mInputUrl.c_str(), "rb"));
  if (!mFile) {
    handleError(outError, "Failed to open a wav file");
    return false;
  }
  mFileLength = getFileSize(mInputUrl);
  if (fread(&riff, 1, sizeof(riff), mFile.get()) != sizeof(riff)) {
    handleError(outError, "Failed to read the wav file's RIFF header");
    return false;
  }
  if (std::string(riff.id, 4) != "RIFF" ||
      std::string(riff.waveFlag, 4) != "WAVE") {
    handleError(outError, "The wav file's RIFF header is broken");
    return false;
  }
  if (fread(&format, 1, sizeof(format), mFile.get()) != sizeof(format)) {
    handleError(outError, "Failed to read wav file's format");
    return false;
  }
  if (std::string(format.id, 4) != "fmt ") {
    handleError(outError, "Failed to find a wav fmt tag");
    return false;
  }
  if (format.formatTag != 1) {
    handleError(outError, "The wav file only support pcm data!");
    return false;
  }
  userDataSize = format.blockSize - sizeof(format) + 8;
  if (userDataSize < 0) {
    handleError(outError, "The wav file's block size is broken!");
    return false;
  } else if (userDataSize > 0) {
    if (fseek(mFile.get(), userDataSize, SEEK_CUR) != 0) {
      handleError(outError, "Failed to seek the wav file to data position");
      return false;
    }
  }

  while (true) {
    if (fread(&data, 1, sizeof(data), mFile.get()) != sizeof(data)) {
      handleError(outError, "Failed to read pcm data from the wav file");
      return false;
    }
    if (std::string(data.id, 4) != "data") {
      if (fseek(mFile.get(), data.dataLength, SEEK_CUR) != 0) {
        handleError(outError, "Failed to read 'data' tag");
        return false;
      }
      continue;
    }
    break;
  }

  mDataOffset = ftell(mFile.get());
  mFileLength = riff.fileLength + 8;
  mDataLength = data.dataLength;
  mChannels = format.channels;
  mSampleRate = format.samplesPerSec;
  mBitsPerSample = format.bitsPerSample;

  auto bytesPerChannel = mBitsPerSample / kBitsPerByte;
  if (bytesPerChannel == sizeof(int16_t)) {
    mSampleFormat = AudioSampleFormat::kInt16;
  } else if (bytesPerChannel == 3) {
    mSampleFormat = AudioSampleFormat::kInt24;
  } else if (bytesPerChannel == sizeof(float)) {
    mSampleFormat = AudioSampleFormat::kFloat;
  }

  mSampleLength = (int64_t) (mDataLength * kBitsPerByte) / (int64_t) (mChannels * mBitsPerSample);
  return true;
}

void WaveFileReader::seekTo(int64_t framePosition) {
  if (!mFile) {
    MINILOG_ERROR("Failed to seek: File is null");
    return;
  }
  if (fseek(mFile.get(), mDataOffset + framePosition, SEEK_SET) != 0) {
    MINILOG_ERROR("Failed to seek: {}", framePosition);
  }
}

void WaveFileReader::close() {
  if (!mFile) {
    return;
  }
  // Release first, then close once. Closing `mFile.get()` and then resetting it made the deleter
  // call fclose on the same FILE a second time, which is the "double free detected in tcache" that
  // ended playback as soon as the reader was destroyed. `release()` hands ownership to this scope,
  // so the deleter has nothing left to close.
  FILE* const file = mFile.release();
  if (fclose(file) == EOF) {
    MINILOG_ERROR("Failed to close wav file");
  }
}

int64_t WaveFileReader::readIntoBuffer(uint8_t* outBuffer,
  int64_t bufferCapacity,
  Error** outError) {
  if (ftell(mFile.get()) >= mDataOffset + mDataLength) {
    return 0;
  }
  auto bytesRead = fread(outBuffer, 1, bufferCapacity, mFile.get());
  auto numFrames = (int64_t) (bytesRead * kBitsPerByte) /
                   (int64_t) (mChannels * mBitsPerSample);
  mFrameLength += numFrames;
  return numFrames;
}

int64_t WaveFileReader::getFileSize(const std::string& url) {
  int64_t size;
  auto file = fopen(url.c_str(), "rb");
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  fclose(file);
  return size;
}

bool WaveFileReader::isWaveFile(const std::string& filename) {
  FILE* fp;

  fp = fopen(filename.c_str(), "rb");
  if (fp == nullptr) {
    return false;
  }

  uint8_t chunkID[4];
  uint8_t format[4];

  fread(&chunkID, sizeof(uint8_t), 4, fp);
  fseek(fp, 8, SEEK_SET);
  fread(&format, sizeof(uint8_t), 4, fp);

  fclose(fp);

  if ((chunkID[0] != 'R' || chunkID[1] != 'I' || chunkID[2] != 'F' || chunkID[3] != 'F') &&
      (format[0] != 'W' || format[1] != 'A' || format[2] != 'V' || format[3] != 'E')) {
    return false;
  } else {
    return true;
  }
}

} // namespace coral
