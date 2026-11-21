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

#include "WmfAudioFileWriter.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <utility>

namespace coral {

namespace {

constexpr int DEFAULT_BIT_RATE = 128000;

} // namespace anonymous

std::unique_ptr<AudioFileWriter>
WmfAudioFileWriter::sniff(const std::string& outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return nullptr;
  }
  if (!isSupportedFileType(fileType)) {
    return nullptr;
  }
  // The platform must be started before the transforms can be enumerated.
  const WmfMediaScope mediaScope;
  if (!mediaScope.isStarted()) {
    return nullptr;
  }

  const auto sampleFormat = format->getSampleFormat();
  if (sampleFormat != AudioSampleFormat::kInt16 &&
      sampleFormat != AudioSampleFormat::kInt32 &&
      sampleFormat != AudioSampleFormat::kFloat) {
    MINILOG_DEBUG("the sink writer does not support the pcm format: {}", convertToText(sampleFormat));
    return nullptr;
  }
  if (format->getSampleRate() <= 0 || format->getChannelCount() <= 0) {
    return nullptr;
  }

  GUID encodedSubType{};
  if (!convertToEncodedSubtype(fileType, encodedSubType)) {
    return nullptr;
  }
  // The sink writer inserts an audio encoder for the compressed file types, the file type is only
  // supported when the platform provides the required encoder.
  if (encodedSubType != MFAudioFormat_PCM &&
      !WmfMediaUtils::isTransformAvailable(false, encodedSubType, false)) {
    MINILOG_DEBUG("no audio encoder is available for the file type: {}", convertToText(fileType));
    return nullptr;
  }

  return std::unique_ptr<AudioFileWriter>(new WmfAudioFileWriter(outputUrl, fileType, format));
}

WmfAudioFileWriter::WmfAudioFileWriter(std::string outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) noexcept
  : AudioFileWriter(format->getSampleFormat(),
      format->getSampleRate(),
      format->getChannelCount()),
    mOutputUrl(std::move(outputUrl)),
    mFileType(fileType),
    mInputFormat(format) {
  mBytesPerFrame = static_cast<int>(
    MediaUtils::getBytesPerFrame(format->getSampleFormat(), format->getChannelCount()));
  mWmfStarted = WmfMediaUtils::startup();
}

WmfAudioFileWriter::~WmfAudioFileWriter() {
  close();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfAudioFileWriter::isSupportedFileType(const MediaFileType& fileType) {
  return fileType == MediaFileType::kMediaFileTypeMpeg4AAC ||
         fileType == MediaFileType::kMediaFileTypeWav;
}

bool WmfAudioFileWriter::convertToContainerType(const MediaFileType& fileType,
  GUID& containerType) {
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC: {
      containerType = MFTranscodeContainerType_MPEG4;
      return true;
    }
    case MediaFileType::kMediaFileTypeWav: {
      containerType = MFTranscodeContainerType_WAVE;
      return true;
    }
    default: {
      return false;
    }
  }
}

bool WmfAudioFileWriter::convertToEncodedSubtype(const MediaFileType& fileType, GUID& subType) {
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC: {
      subType = MFAudioFormat_AAC;
      return true;
    }
    case MediaFileType::kMediaFileTypeWav: {
      subType = MFAudioFormat_PCM;
      return true;
    }
    default: {
      return false;
    }
  }
}

Microsoft::WRL::ComPtr<IMFMediaType> WmfAudioFileWriter::createInputMediaType() const {
  GUID sampleSubType{};
  if (!WmfMediaUtils::sampleFormatToSubtype(getSampleFormat(), sampleSubType)) {
    return nullptr;
  }
  return WmfMediaUtils::createPcmMediaType(sampleSubType,
    static_cast<int>(getSampleRate()),
    static_cast<int>(getChannelCount()));
}

Microsoft::WRL::ComPtr<IMFMediaType> WmfAudioFileWriter::createOutputMediaType() const {
  GUID encodedSubType{};
  if (!convertToEncodedSubtype(mFileType, encodedSubType)) {
    return nullptr;
  }
  if (encodedSubType == MFAudioFormat_PCM) {
    GUID sampleSubType{};
    if (!WmfMediaUtils::sampleFormatToSubtype(getSampleFormat(), sampleSubType)) {
      return nullptr;
    }
    return WmfMediaUtils::createPcmMediaType(sampleSubType,
      static_cast<int>(getSampleRate()),
      static_cast<int>(getChannelCount()));
  }

  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  if (FAILED(::MFCreateMediaType(&mediaType)) || mediaType == nullptr) {
    return nullptr;
  }

  auto bitrate = mInputFormat->getBitrate();
  if (bitrate == MediaFormat::NO_VALUE || bitrate <= 0) {
    bitrate = DEFAULT_BIT_RATE;
  }

  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  mediaType->SetGUID(MF_MT_SUBTYPE, encodedSubType);
  mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(getSampleRate()));
  mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(getChannelCount()));
  mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(bitrate / 8));
  mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);
  mediaType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));

  if (encodedSubType == MFAudioFormat_AAC) {
    mediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    mediaType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
  }
  return mediaType;
}

bool WmfAudioFileWriter::open(Error** outError) {
  if (mOpened) {
    return true;
  }
  if (mOutputUrl.empty()) {
    handleError(outError, "Failed to open the audio file writer", "The output url is empty");
    return false;
  }
  if (!mWmfStarted) {
    handleError(outError, "Failed to open the audio file writer",
      "The media foundation platform is not available");
    return false;
  }

  GUID containerType{};
  if (!convertToContainerType(mFileType, containerType)) {
    handleError(outError, "Failed to open the audio file writer", "Unsupported file type");
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  auto result = ::MFCreateAttributes(&attributes, 3);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "MFCreateAttributes failed");
    return false;
  }
  attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, containerType);
  attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  // the samples are written as fast as possible, the caller controls the pacing.
  attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

  const auto wideUrl = WmfMediaUtils::toSinkWriterUrl(mOutputUrl);
  result = ::MFCreateSinkWriterFromURL(wideUrl.c_str(), nullptr, attributes.Get(), &mSinkWriter);
  if (FAILED(result) || mSinkWriter == nullptr) {
    mSinkWriter.Reset();
    WmfMediaUtils::handleHresult(outError, result, "MFCreateSinkWriterFromURL failed",
      mOutputUrl);
    return false;
  }

  const auto outputType = createOutputMediaType();
  if (outputType == nullptr) {
    handleError(outError, "Failed to open the audio file writer", "Invalid output media type");
    return false;
  }
  result = mSinkWriter->AddStream(outputType.Get(), &mStreamIndex);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "IMFSinkWriter::AddStream failed");
    return false;
  }

  const auto inputType = createInputMediaType();
  if (inputType == nullptr) {
    handleError(outError, "Failed to open the audio file writer", "Invalid input media type");
    return false;
  }
  result = mSinkWriter->SetInputMediaType(mStreamIndex, inputType.Get(), nullptr);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "IMFSinkWriter::SetInputMediaType failed");
    return false;
  }

  result = mSinkWriter->BeginWriting();
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "IMFSinkWriter::BeginWriting failed");
    return false;
  }

  mFramePosition = 0;
  mOpened = true;
  return true;
}

int64_t WmfAudioFileWriter::writeFromPCMBuffer(void* inputBuffer,
  const size_t numFrames,
  Error** outError) {
  if (!mOpened || mSinkWriter == nullptr) {
    handleError(outError, "Failed to write the audio samples", "The writer is not open");
    return Definitions::kInvalidValue;
  }
  if (inputBuffer == nullptr || numFrames == 0) {
    return 0;
  }

  const auto size = static_cast<size_t>(mBytesPerFrame) * numFrames;
  const auto timeUs = mFramePosition * Definitions::kMicrosPerSecond /
                      static_cast<int64_t>(getSampleRate());
  auto sample = WmfMediaUtils::createSample(static_cast<const uint8_t*>(inputBuffer), size, timeUs);
  if (sample == nullptr) {
    handleError(outError, "Failed to write the audio samples", "Failed to create the sample");
    return Definitions::kInvalidValue;
  }
  const auto durationUs =
    static_cast<int64_t>(numFrames) * Definitions::kMicrosPerSecond / static_cast<int64_t>(getSampleRate());
  sample->SetSampleDuration(durationUs * 10); // us to 100ns

  const auto result = mSinkWriter->WriteSample(mStreamIndex, sample.Get());
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "IMFSinkWriter::WriteSample failed");
    return Definitions::kInvalidValue;
  }

  mFramePosition += static_cast<int64_t>(numFrames);
  return static_cast<int64_t>(numFrames);
}

void WmfAudioFileWriter::close() {
  if (mSinkWriter != nullptr) {
    if (mOpened) {
      const auto result = mSinkWriter->Finalize();
      if (FAILED(result)) {
        WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::Finalize failed");
      }
    }
    mSinkWriter.Reset();
  }
  mOpened = false;
}

} // namespace coral
