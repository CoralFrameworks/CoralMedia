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

#include "WaveFileWriter.h"

#include <media/Utilities.h>
#include <minilog/log.h>

#include <sys/stat.h>

#include <iostream>

namespace coral {

std::unique_ptr<WaveFileWriter>
WaveFileWriter::sniff(const std::string &outputUrl, const std::shared_ptr<MediaFormat> &format) {
  if (format->getSampleFormat() != AudioSampleFormat::kInt16
      && format->getSampleFormat() != AudioSampleFormat::kInt24) {
    MINILOG_ERROR("Unsupported format: {}", convertToText(format->getSampleFormat()));
    return nullptr;
  }
  return std::make_unique<WaveFileWriter>(outputUrl,
      format->getSampleRate(),
      format->getChannelCount(),
      format->getSampleFormat());
}

WaveFileWriter::WaveFileWriter(const std::string &outputUrl,
    const uint64_t sampleRate,
    const uint32_t channelCount,
    const AudioSampleFormat sampleFormat) noexcept
    : AudioFileWriter(sampleFormat, sampleRate, channelCount),
      mOutputUrl(outputUrl),
      mFile(nullptr),
      mBitsPerSample(static_cast<int32_t>(MediaUtils::getBytesPerSample(sampleFormat) * BITS_PER_BYTE)),
      mBytesPerFrame(static_cast<int32_t>(MediaUtils::getBytesPerFrame(sampleFormat, channelCount))),
      bytesWritten(0),
      headerWritten(false) {
}

WaveFileWriter::~WaveFileWriter() {
  WaveFileWriter::close();
}

bool WaveFileWriter::open(Error **outError) {
  if (mOutputUrl.empty()) {
    handleError(outError, "The output url is empty");
    return false;
  }
  mFile = fopen(mOutputUrl.c_str(), "wb");
  if (mFile == nullptr) {
    handleError(outError, "Failed to open the output file", mOutputUrl.c_str());
    return false;
  }
  bytesWritten = 0;
  headerWritten = false;
  return true;
}

void WaveFileWriter::close() {
  if (mFile != nullptr) {
    if (headerWritten) {
      updateHeaderInformation();
    }
    fclose(mFile);
    mFile = nullptr;
  }
}

int64_t WaveFileWriter::writeFromPCMBuffer(void* inputBuffer, size_t numFrames, Error** outError) {
  if (mFile == nullptr) {
    handleError(outError, "Failed to write the audio samples", "The writer is not open");
    return -1;
  }
  if (inputBuffer == nullptr) {
    handleError(outError, "Failed to write the audio samples", "The input buffer is null");
    return -1;
  }

  // The pcm data is interleaved, so a frame contains one sample per channel.
  const auto numSamples = numFrames * getSamplesPerFrame();
  auto sampleFormat = getSampleFormat();
  if (sampleFormat == AudioSampleFormat::kInt16) {
    writeIntSamples<int16_t>(inputBuffer, numSamples, 1.0f / 32768.0f);
  } else if (sampleFormat == AudioSampleFormat::kFloat) {
    write(static_cast<const float *>(inputBuffer), 0, numSamples);
  } else if (sampleFormat == AudioSampleFormat::kInt24) {
    auto *p = static_cast<const int32_t *>(inputBuffer);
    for (size_t i = 0; i < numSamples; ++i) {
      write(((p[i] << 8) >> 8) / 8388608.0f);
    }
  } else if (sampleFormat == AudioSampleFormat::kInt32) {
    writeIntSamples<int32_t>(inputBuffer, numSamples, 1.0f / 2147483648.0f);
  } else {
    return -1;
  }

  return numFrames;
}

//--------------------------------------------------------------------------------------------------

void WaveFileWriter::write(const float value) {
  if (!headerWritten) {
    writeHeader();
  }
  if (getBitsPerSample() == 24) {
    writePCM24(value);
  } else {
    writePCM16(value);
  }
}

void WaveFileWriter::write(const float *buffer, const size_t startSample, const size_t numSamples) {
  for (int32_t i = 0; i < numSamples; i++) {
    write(buffer[startSample + i]);
  }
}

void WaveFileWriter::writeIntLittle(int32_t n) {
  writeByte(n);
  writeByte(n >> 8);
  writeByte(n >> 16);
  writeByte(n >> 24);
}

void WaveFileWriter::writeShortLittle(int16_t n) {
  writeByte(n);
  writeByte(n >> 8);
}

void WaveFileWriter::writeFormatChunk() {
  int32_t bytesPerSample = (getBitsPerSample() + 7) / 8;

  writeByte('f');
  writeByte('m');
  writeByte('t');
  writeByte(' ');
  writeIntLittle(16); // chunk size
  writeShortLittle(WAVE_FORMAT_PCM);
  writeShortLittle((int16_t)getSamplesPerFrame());
  writeIntLittle(getSampleRate());
  // bytes/second
  writeIntLittle(getSampleRate() * getSamplesPerFrame() * bytesPerSample);
  // block align
  writeShortLittle((int16_t)(getSamplesPerFrame() * bytesPerSample));
  writeShortLittle((int16_t)getBitsPerSample());
}

void WaveFileWriter::writeDataChunkHeader(const int64_t dataSize) {
  writeByte('d');
  writeByte('a');
  writeByte('t');
  writeByte('a');
  // Maximum size is not strictly correct but is commonly used
  // when we do not know the final size.
  writeIntLittle(dataSize >= 0 && dataSize < INT32_MAX ? static_cast<int32_t>(dataSize) : INT32_MAX);
}

void WaveFileWriter::writeHeader() {
  const auto totalBytes = bytesWritten;
  writeRiffHeader(INT32_MAX);
  writeFormatChunk();
  writeDataChunkHeader(INT32_MAX);
  bytesWritten = totalBytes;
  headerWritten = true;
}

void WaveFileWriter::writeByte(uint8_t b) {
  auto written = fwrite(&b, sizeof(uint8_t), 1, mFile);
  if (written != 1) {
    return;
  }
  bytesWritten += 1;
}

void WaveFileWriter::writePCM24(float value) {
  // Offset before casting so that we can avoid using floor().
  // Also round by adding 0.5 so that very small signals go to zero.
  float temp = (PCM24_MAX * value) + 0.5 - PCM24_MIN;
  int32_t sample = ((int)temp) + PCM24_MIN;
  // clip to 24-bit range
  if (sample > PCM24_MAX) {
    sample = PCM24_MAX;
  } else if (sample < PCM24_MIN) {
    sample = PCM24_MIN;
  }
  // encode as little-endian
  writeByte(sample); // little end
  writeByte(sample >> 8); // middle
  writeByte(sample >> 16); // big end
}

void WaveFileWriter::writePCM16(float value) {
  // Offset before casting so that we can avoid using floor().
  // Also round by adding 0.5 so that very small signals go to zero.
  float temp = (INT16_MAX * value) + 0.5 - INT16_MIN;
  int32_t sample = ((int)temp) + INT16_MIN;
  if (sample > INT16_MAX) {
    sample = INT16_MAX;
  } else if (sample < INT16_MIN) {
    sample = INT16_MIN;
  }
  writeByte(sample); // little end
  writeByte(sample >> 8); // big end
}

void WaveFileWriter::writeRiffHeader(const int64_t fileLength) {
  writeByte('R');
  writeByte('I');
  writeByte('F');
  writeByte('F');
  // Maximum size is not strictly correct but is commonly used
  // when we do not know the final size.
  writeIntLittle(fileLength >= 0 && fileLength < INT32_MAX ? static_cast<int32_t>(fileLength) : INT32_MAX);
  writeByte('W');
  writeByte('A');
  writeByte('V');
  writeByte('E');
}

void WaveFileWriter::updateHeaderInformation() {
  // seek to start
  fseek(mFile, 0, SEEK_SET);
  // bytesWritten counts the data chunk only, the canonical header is 44 bytes.
  const auto dataBytes = bytesWritten;
  writeRiffHeader(dataBytes + WAV_HEADER_SIZE - RIFF_HEADER_SIZE);
  writeFormatChunk();
  writeDataChunkHeader(dataBytes);
  bytesWritten = dataBytes;
  fflush(mFile);
}

int64_t WaveFileWriter::getSampleLength() const {
  return getFramePosition();
}

int64_t WaveFileWriter::getFramePosition() const {
  if (mBytesPerFrame <= 0) {
    return 0;
  }
  return bytesWritten / mBytesPerFrame;
}

} // namespace coral
