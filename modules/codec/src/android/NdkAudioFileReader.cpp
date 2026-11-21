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

#include "NdkAudioFileReader.h"

#include <media/Definitions.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <core/jni/android/AudioFormat.h>

#include <fcntl.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace coral {

std::unique_ptr<NdkAudioFileReader> NdkAudioFileReader::sniff(const std::string& inputUrl) {
  auto extractor = createExtractor(inputUrl);
  if (extractor == nullptr) {
    return nullptr;
  }
  int streamIndex = -1;
  auto codec = createCodec(extractor, &streamIndex);
  if (codec == nullptr) {
    AMediaExtractor_delete(extractor);
    return nullptr;
  }
  return std::make_unique<NdkAudioFileReader>(inputUrl, extractor, codec, streamIndex);
}

void NdkAudioFileReader::prepareAudioFormats() {
  if (!mExtractor) {
    return;
  }

  auto format = AMediaExtractor_getTrackFormat(mExtractor.get(), mStreamIndex);
  int intValue;
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &intValue)) {
    mSampleRate = intValue;
  }

  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &intValue)) {
    mChannelCount = intValue;
  }

  // parse pcm encoding
  mSampleFormat = AudioSampleFormat::kInt16;
  if (AMediaFormat_getInt32(format, "pcm-encoding", &intValue)) {
    switch (intValue) {
      case JniAudioFormat::ENCODING_PCM_16BIT: {
        mSampleFormat = AudioSampleFormat::kInt16;
        break;
      }
      case JniAudioFormat::ENCODING_PCM_FLOAT: {
        mSampleFormat = AudioSampleFormat::kFloat;
        break;
      }
      case JniAudioFormat::ENCODING_PCM_24BIT_PACKED: {
        mSampleFormat = AudioSampleFormat::kInt24;
        break;
      }
      case JniAudioFormat::ENCODING_PCM_32BIT: {
        mSampleFormat = AudioSampleFormat::kInt32;
        break;
      }
      default: {
        break;
      }
    }
  }
  int64_t longValue;
  if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &longValue)) {
    auto rt = std::div((int64_t) (longValue * mSampleRate), Definitions::kMicrosPerSecond);
    mSampleLength = rt.quot;
  }
  mBytesPerFrame = MediaUtils::getBytesPerFrame(mSampleFormat, mChannelCount);

  AMediaFormat_delete(format);
}

AMediaExtractor*
NdkAudioFileReader::createExtractor(const std::string& inputUrl) {

  struct stat statBuffer{};
  if (stat(inputUrl.c_str(), &statBuffer) != 0) {
    MINILOG_ERROR("Failed to state file: {}, error: {}", inputUrl, strerror(errno));
    return nullptr;
  }

  // open a local file url
  auto fd = ::open(inputUrl.c_str(), O_RDONLY);
  if (fd < 0) {
    return nullptr;
  }

  // creates an AMediaExtractor
  auto extractor = AMediaExtractor_new();
  auto status = AMediaExtractor_setDataSourceFd(extractor, fd, 0, statBuffer.st_size);
  ::close(fd);
  if (status != AMEDIA_OK) {
    AMediaExtractor_delete(extractor);
    return nullptr;
  }
  return extractor;
}

AMediaCodec*
NdkAudioFileReader::createCodec(AMediaExtractor* extractor, int* streamIndex) {
  AMediaCodec* codec = nullptr;
  media_status_t err;
  if (extractor == nullptr) {
    return nullptr;
  }

  // creates and configure
  auto tracks = AMediaExtractor_getTrackCount(extractor);
  for (auto i = 0; i < tracks; ++i) {
    AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor, i);
    if (format == nullptr) {
      continue;
    }
    const char* mime = nullptr;
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
      AMediaFormat_delete(format);
      break;
    } else if (!strncmp(mime, "audio/", 6)) {
      AMediaExtractor_selectTrack(extractor, i);
      codec = AMediaCodec_createDecoderByType(mime);
      err = AMediaCodec_configure(codec, format, nullptr, nullptr, 0);
      if (err != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        codec = nullptr;
      } else {
        AMediaCodec_start(codec);
      }
      AMediaFormat_delete(format);
      if (streamIndex) {
        *streamIndex = i;
      }
      break;
    } else {
      AMediaFormat_delete(format);
    }
  }

  // Failed to find and configure codec
  if (codec == nullptr) {
    return nullptr;
  }

  return codec;
}

NdkAudioFileReader::NdkAudioFileReader(std::string inputUrl,
  AMediaExtractor* extractor,
  AMediaCodec* codec,
  int streamIndex) noexcept
  : mInputUrl(std::move(inputUrl)),
    mStreamIndex(streamIndex),
    mSampleLength(Definitions::kInvalidValue),
    mSampleRate(0),
    mChannelCount(0),
    mFramePosition(Definitions::kPositionUnset),
    mSampleFormat(AudioSampleFormat::kDefault),
    mBytesPerFrame(0),
    mPrepared(false),
    mInputEof(false),
    mOutputEof(false) {
  mCacheBuffer.flip();
  if (extractor && codec) {
    mExtractor.reset(extractor);
    mMediaCodec.reset(codec);
    mPrepared = true;
  } else {
    if (extractor) {
      AMediaExtractor_delete(extractor);
    }
    if (codec) {
      AMediaCodec_delete(codec);
    }
    mPrepared = false;
  }
  prepareAudioFormats();
}

int64_t NdkAudioFileReader::getSampleLength() const { return mSampleLength; }

int64_t NdkAudioFileReader::getSampleRate() const { return mSampleRate; }

int32_t NdkAudioFileReader::getChannelCount() const { return mChannelCount; }

int64_t NdkAudioFileReader::getFramePosition() const { return mFramePosition; }

AudioSampleFormat NdkAudioFileReader::getSampleFormat() const { return mSampleFormat; }

bool NdkAudioFileReader::open(Error** outError) {
  if (mPrepared) {
    return true;
  }
  mExtractor.reset(createExtractor(mInputUrl));
  if (mExtractor) {
    mMediaCodec.reset(createCodec(mExtractor.get(), &mStreamIndex));
    if (!mMediaCodec) {
      handleError(outError, "Failed to alloc the audio decoder!");
      return false;
    }
  } else {
    handleError(outError, "Failed to alloc the audio extractor!");
    mMediaCodec.reset(nullptr);
    return false;
  }
  prepareAudioFormats();
  mPrepared = true;
  mInputEof = false;
  mOutputEof = false;
  mCacheBuffer.clear();
  mCacheBuffer.flip();
  mFramePosition = 0;
  return true;
}

void NdkAudioFileReader::seekTo(int64_t framePosition) {
  mFramePosition = framePosition;
  auto timeUs =
    std::div(framePosition * Definitions::kMicrosPerSecond, mSampleRate).quot;
  if (mExtractor) {
    AMediaExtractor_seekTo(mExtractor.get(), timeUs,
      AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
  }
  if (mMediaCodec) {
    AMediaCodec_flush(mMediaCodec.get());
  }
  mCacheBuffer.clear();
  mCacheBuffer.flip();
  mInputEof = false;
  mOutputEof = false;
}

void NdkAudioFileReader::close() {
  if (mMediaCodec) {
    AMediaCodec_stop(mMediaCodec.get());
    mMediaCodec.reset();
  }
  if (mExtractor) {
    mExtractor.reset();
  }
  mPrepared = false;
  mInputEof = true;
  mOutputEof = true;
  mCacheBuffer.clear();
  mCacheBuffer.flip();
}

bool NdkAudioFileReader::feedInputBuffer() {
  const ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(mMediaCodec.get(), 0);
  if (inputIndex < 0) {
    // Every input buffer is in use: draining the decoder's output frees one.
    return false;
  }

  size_t bufferSize = 0;
  auto* inputBuffer = AMediaCodec_getInputBuffer(mMediaCodec.get(), inputIndex, &bufferSize);
  if (inputBuffer == nullptr) {
    // A codec that has no input buffer to write into cannot consume the stream any further.
    AMediaCodec_queueInputBuffer(mMediaCodec.get(), inputIndex, 0, 0, 0,
      AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    mInputEof = true;
    return false;
  }
  if (bufferSize == 0) {
    // Nothing fits into this input buffer; hand it back untouched so a later call can use it.
    AMediaCodec_queueInputBuffer(mMediaCodec.get(), inputIndex, 0, 0, 0, 0);
    return false;
  }

  const auto sampleSize = AMediaExtractor_readSampleData(mExtractor.get(), inputBuffer, bufferSize);
  const auto presentationTimeUs = AMediaExtractor_getSampleTime(mExtractor.get());
  const bool endOfStream = sampleSize <= 0;
  AMediaCodec_queueInputBuffer(mMediaCodec.get(), inputIndex, 0,
    endOfStream ? 0 : static_cast<size_t>(sampleSize), presentationTimeUs,
    endOfStream ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
  if (endOfStream) {
    mInputEof = true;
  } else {
    AMediaExtractor_advance(mExtractor.get());
  }
  return true;
}

bool NdkAudioFileReader::refreshOutputFormat() {
  auto* format = AMediaCodec_getOutputFormat(mMediaCodec.get());
  if (format == nullptr) {
    return false;
  }

  int32_t value = 0;
  auto channels = mChannelCount;
  auto sampleRate = mSampleRate;
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &value) && value > 0) {
    channels = value;
  }
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &value) && value > 0) {
    sampleRate = value;
  }

  auto sampleFormat = mSampleFormat;
  if (AMediaFormat_getInt32(format, "pcm-encoding", &value)) {
    switch (value) {
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
        break;
      }
    }
  }
  AMediaFormat_delete(format);

  if (channels != mChannelCount || sampleRate != mSampleRate || sampleFormat != mSampleFormat) {
    MINILOG_DEBUG("The audio decoder changed its output format: {} Hz, {} channels, format {}",
      static_cast<long long>(sampleRate), channels, static_cast<int>(sampleFormat));
  }
  const auto bytesPerFrame = MediaUtils::getBytesPerFrame(sampleFormat, channels);
  const bool changed = bytesPerFrame != mBytesPerFrame;
  mChannelCount = channels;
  mSampleRate = sampleRate;
  mSampleFormat = sampleFormat;
  mBytesPerFrame = bytesPerFrame;
  return changed;
}

int64_t NdkAudioFileReader::readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) {
  if (mExtractor == nullptr || mMediaCodec == nullptr) {
    handleError(outError, "Failed to open audio file.");
    return Definitions::kInvalidValue;
  }
  if (outBuffer == nullptr || bufferCapacity <= 0 || mBytesPerFrame == 0) {
    handleError(outError, "The audio decode buffer is invalid.");
    return Definitions::kInvalidValue;
  }

  // The caller asks for bytes, but the frames it gets back are whole frames only.
  auto capacity = static_cast<size_t>(bufferCapacity) / mBytesPerFrame * mBytesPerFrame;
  if (capacity == 0) {
    return 0;
  }

  size_t written = 0;

  // 1. The part of the previous decoder output that did not fit into the caller's buffer first.
  //    It is counted here, otherwise the bytes it holds would be handed out twice or lost.
  if (mCacheBuffer.hasRemaining()) {
    const auto cached = std::min(mCacheBuffer.remaining(), capacity);
    mCacheBuffer.get(outBuffer, cached);
    written += cached;
    if (!mCacheBuffer.hasRemaining()) {
      mCacheBuffer.clear();
      mCacheBuffer.flip();
    }
  }

  // 2. Decode until the caller's buffer is full, or the stream has no more audio.
  size_t idleRounds = 0;
  while (written < capacity && !mOutputEof) {
    if (!mInputEof) {
      (void) feedInputBuffer();
    }

    bool progressed = false;
    AMediaCodecBufferInfo info{};
    const ssize_t status = AMediaCodec_dequeueOutputBuffer(mMediaCodec.get(), &info, TIME_OUT_US);
    if (status >= 0) {
      progressed = true;
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
        mOutputEof = true;
      }
      if (info.size > 0) {
        const auto* outputBuffer = AMediaCodec_getOutputBuffer(mMediaCodec.get(), status, nullptr);
        if (outputBuffer == nullptr) {
          AMediaCodec_releaseOutputBuffer(mMediaCodec.get(), status, false);
          MINILOG_ERROR("Failed to get the output buffer of the audio decoder");
          break;
        }
        // The valid samples start at info.offset, which a decoder does not have to leave at 0.
        const auto* data = outputBuffer + info.offset;
        const auto size = static_cast<size_t>(info.size);
        const auto toCopy = std::min(size, capacity - written);
        std::memcpy(outBuffer + written, data, toCopy);
        written += toCopy;
        if (toCopy < size) {
          // The rest of the decoded buffer is delivered by the next read.
          mCacheBuffer.clear();
          mCacheBuffer.put(data + toCopy, size - toCopy);
          mCacheBuffer.flip();
        }
      }
      AMediaCodec_releaseOutputBuffer(mMediaCodec.get(), status, false);
    } else if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      // The codec may only now reveal the real channel count or sample format of the stream.
      const bool frameSizeChanged = refreshOutputFormat();
      if (frameSizeChanged) {
        if (written > 0) {
          // What has already been written is in the previous frame size: hand it back now and
          // let the next read continue in the format the decoder switched to.
          break;
        }
        if (mBytesPerFrame == 0) {
          break;
        }
        capacity = static_cast<size_t>(bufferCapacity) / mBytesPerFrame * mBytesPerFrame;
        if (capacity == 0) {
          break;
        }
      }
    } else if (status == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      // The codec replaced its buffers; the next dequeue uses the new set.
    } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      // The decoder has no output for us yet.
    } else {
      MINILOG_ERROR("Failed to dequeue the output buffer of the audio decoder: {}", static_cast<int>(status));
      break;
    }

    if (progressed) {
      idleRounds = 0;
    } else if (++idleRounds > kMaxIdleRounds) {
      // Never block the producer thread forever on a decoder that stops answering.
      MINILOG_WARN("The audio decoder produced no output in {} tries", kMaxIdleRounds);
      break;
    }
  }

  const auto frames = static_cast<int64_t>(written / mBytesPerFrame);
  if (frames > 0) {
    mFramePosition += frames;
  }
  if (mOutputEof && !mCacheBuffer.hasRemaining() && mSampleLength > 0) {
    mFramePosition = mSampleLength;
  }
  return frames;
}

} // namespace coral
