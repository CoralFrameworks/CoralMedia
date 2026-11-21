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

#include "WmfMediaExtractor.h"

#include "WmfExtractorByteStream.h"
#include "WmfMediaUtils.h"

#include <media/Definitions.h>

#include <minilog/log.h>

#include <utility>

namespace coral {

std::shared_ptr<WmfMediaExtractor> WmfMediaExtractor::createWithUrl(const std::string& url) {
  auto extractor = std::shared_ptr<WmfMediaExtractor>(new WmfMediaExtractor(url));
  if (!extractor->setDataSource(nullptr)) {
    return nullptr;
  }
  return extractor;
}

std::shared_ptr<WmfMediaExtractor> WmfMediaExtractor::createForInput(std::string url) {
  // Nothing is opened here: the media is the one of the reader, which is given to the extractor when
  // it is asked to recognize the container.
  return std::shared_ptr<WmfMediaExtractor>(new WmfMediaExtractor(std::move(url)));
}

WmfMediaExtractor::WmfMediaExtractor(std::string url)
  : mUrl(std::move(url)) {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfMediaExtractor::~WmfMediaExtractor() {
  clear();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

int WmfMediaExtractor::sniff(AVExtractorInput& input) {
  if (mReader) {
    // The media was opened by the url this extractor was created with, so it reads itself and it
    // recognizes the container it was created for.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  // The media is the one of the reader: the source reader is created over the input, so the bytes it
  // reads are the bytes the reader measures, see WmfExtractorByteStream. A media the source reader
  // cannot be created for is a container the platform does not read.
  if (!openFromInput(input, nullptr)) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

bool WmfMediaExtractor::setDataSource(Error** outError) {
  if (mReader) {
    return true;
  }
  const auto wideUrl = WmfMediaUtils::utf8ToWide(mUrl);
  if (wideUrl.empty()) {
    WmfMediaUtils::handleHresult(outError, E_INVALIDARG, "The url is empty", mUrl);
    return false;
  }
  return createReader(wideUrl.c_str(), nullptr, outError);
}

bool WmfMediaExtractor::openFromInput(AVExtractorInput& input, Error** outError) {
  if (mReader) {
    return true;
  }
  mInput = std::make_unique<AVMediaExtractorInput>(input);
  auto byteStream = Microsoft::WRL::Make<WmfExtractorByteStream>(*mInput);
  if (byteStream == nullptr) {
    mInput.reset();
    WmfMediaUtils::handleHresult(outError, E_OUTOFMEMORY,
        "The byte stream of the media of the reader could not be created", mUrl);
    return false;
  }
  if (!createReader(nullptr, byteStream.Get(), outError)) {
    mInput.reset();
    return false;
  }
  return true;
}

bool WmfMediaExtractor::createReader(const wchar_t* url, IMFByteStream* byteStream,
    Error** outError) {
  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  auto result = ::MFCreateAttributes(&attributes, 2);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "MFCreateAttributes failed");
    return false;
  }
  // The extractor needs the compressed samples, so the video processing is not enabled and the
  // hardware transforms are not used to decode the stream.
  attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
  attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);

  if (byteStream != nullptr) {
    result = ::MFCreateSourceReaderFromByteStream(byteStream, attributes.Get(), &mReader);
  } else {
    result = ::MFCreateSourceReaderFromURL(url, attributes.Get(), &mReader);
  }
  if (FAILED(result) || !mReader) {
    mReader.Reset();
    WmfMediaUtils::handleHresult(outError, result,
        byteStream != nullptr ? "MFCreateSourceReaderFromByteStream failed"
                              : "MFCreateSourceReaderFromURL failed",
        mUrl);
    return false;
  }

  PROPVARIANT duration;
  ::PropVariantInit(&duration);
  if (SUCCEEDED(mReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
    MF_PD_DURATION, &duration)) &&
      duration.vt == VT_UI8) {
    // MF_PD_DURATION is expressed in 100ns units.
    mDurationUs = static_cast<int64_t>(duration.uhVal.QuadPart) / 10;
  } else {
    mDurationUs = Definitions::kTimeUnset;
  }
  ::PropVariantClear(&duration);

  mSeekable = mDurationUs != Definitions::kTimeUnset;
  return true;
}

void WmfMediaExtractor::init(AVExtractorOutput& output) {
  mOutput = &output;
  if (!prepareTracks()) {
    return;
  }
  mPrepared = true;
  mOutput->endTracks();
  // The extractor is the seek map of the media it reads, and it is owned by a shared pointer, so the
  // output can keep it alive for as long as it refers to it.
  mOutput->seekMap(shared_from_this());
}

bool WmfMediaExtractor::prepareTracks() {
  if (!mReader || mOutput == nullptr) {
    return false;
  }

  // Deselect every stream first, only the audio/video streams which are supported by the
  // extractor are selected.
  mReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

  for (DWORD index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IMFMediaType> nativeType;
    const auto result = mReader->GetNativeMediaType(index, 0, &nativeType);
    if (FAILED(result) || nativeType == nullptr) {
      break;
    }

    GUID majorType{};
    if (FAILED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType))) {
      continue;
    }
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    if (majorType == MFMediaType_Audio) {
      mediaType = MediaType::kMediaTypeAudio;
    } else if (majorType == MFMediaType_Video) {
      mediaType = MediaType::kMediaTypeVideo;
    } else {
      continue;
    }

    auto format = WmfMediaUtils::convertToMediaFormat(nativeType.Get(), true);
    if (format == nullptr || format->getMimeType().empty()) {
      MINILOG_DEBUG("skip the stream {}: unsupported media type", index);
      continue;
    }
    if (mediaType == MediaType::kMediaTypeAudio &&
        (format->getSampleRate() <= 0 || format->getChannelCount() <= 0)) {
      MINILOG_DEBUG("skip the stream {}: invalid audio format", index);
      continue;
    }

    const auto trackId = static_cast<int>(index);
    auto trackOutput = mOutput->track(trackId, mediaType, AVTrackOutput::emptyConsumer());
    if (trackOutput == nullptr) {
      continue;
    }

    HRESULT setTypeResult = S_OK;
    bool decodedAudio = false;
    if (mediaType == MediaType::kMediaTypeAudio &&
        format->getMimeType() == MimeTypes::AUDIO_MPEG) {
      // The source reader can decode the MP3 audio of an AVI file, but the Media Foundation MP3
      // decoder changes its output sample rate while it reads the stream. Asking the reader for PCM
      // keeps the audio on the rate of the file and avoids the renegotiation in the renderer.
      Microsoft::WRL::ComPtr<IMFMediaType> pcmType;
      if (SUCCEEDED(::MFCreateMediaType(&pcmType)) && pcmType != nullptr) {
        const auto channels = static_cast<UINT32>(format->getChannelCount());
        const auto sampleRate = static_cast<UINT32>(format->getSampleRate());
        pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        pcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2);
        setTypeResult = mReader->SetCurrentMediaType(index, nullptr, pcmType.Get());
        if (SUCCEEDED(setTypeResult)) {
          Microsoft::WRL::ComPtr<IMFMediaType> actualType;
          if (SUCCEEDED(mReader->GetCurrentMediaType(index, &actualType)) && actualType != nullptr) {
            auto pcmFormat = WmfMediaUtils::convertToMediaFormat(actualType.Get(), true);
            if (pcmFormat != nullptr) {
              format = std::move(pcmFormat);
              decodedAudio = true;
            }
          }
        }
      }
    }
    if (!decodedAudio) {
      // Requests the compressed data of the stream, without this the source reader inserts a
      // decoder and delivers the decoded PCM/video frames.
      setTypeResult = mReader->SetCurrentMediaType(index, nullptr, nativeType.Get());
    }
    if (FAILED(setTypeResult)) {
      MINILOG_DEBUG("failed to select the media type of the stream {}, hr: {}",
        index, WmfMediaUtils::hresultToString(setTypeResult));
    }

    Track track;
    track.streamIndex = index;
    track.trackId = trackId;
    track.mediaType = mediaType;
    track.output = std::move(trackOutput);
    track.selected = true;

    // Reports the format to the caller before the samples are delivered.
    track.output->format(format);
    mTracks.emplace_back(std::move(track));

    if (SUCCEEDED(setTypeResult)) {
      mReader->SetStreamSelection(index, TRUE);
    }
  }

  return !mTracks.empty();
}

AVMediaExtractor::PlatformResult WmfMediaExtractor::read(PositionHolder& seekPosition) {
  static_cast<void>(seekPosition);
  if (!mReader || mOutput == nullptr || !mPrepared) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  int retry = 0;
  while (retry++ < MAX_READ_RETRY) {
    // Deliver the pending sample with the earliest presentation timestamp first, so the samples of
    // the different streams stay interleaved.
    Track* earliest = nullptr;
    for (auto& track: mTracks) {
      if (!track.pendingSample) {
        continue;
      }
      if (earliest == nullptr || track.pendingTimeUs < earliest->pendingTimeUs) {
        earliest = &track;
      }
    }
    if (earliest != nullptr) {
      emitSample(*earliest);
      return PlatformResult::RESULT_CONTINUE;
    }

    if (!readPendingSample()) {
      return PlatformResult::RESULT_END_OF_INPUT;
    }
  }

  return PlatformResult::RESULT_CONTINUE;
}

bool WmfMediaExtractor::readPendingSample() {
  Track* target = nullptr;
  for (auto& track: mTracks) {
    if (track.selected && !track.endOfStream) {
      target = &track;
      break;
    }
  }
  if (target == nullptr) {
    return false;
  }

  DWORD streamFlags = 0;
  LONGLONG timestamp = 0;
  Microsoft::WRL::ComPtr<IMFSample> sample;
  const auto result = mReader->ReadSample(target->streamIndex, 0, nullptr, &streamFlags,
    &timestamp, &sample);
  if (FAILED(result)) {
    MINILOG_ERROR("ReadSample of the stream {} failed: {}", target->streamIndex,
      WmfMediaUtils::hresultToString(result));
    target->endOfStream = true;
    return true;
  }

  if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
    target->endOfStream = true;
    return true;
  }
  if ((streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
    MINILOG_DEBUG("the media type of the stream {} changed", target->streamIndex);
  }
  if (sample == nullptr) {
    return true;
  }

  UINT32 cleanPoint = 0;
  sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

  target->pendingSample = sample;
  target->pendingTimeUs = timestamp / 10; // 100ns to us
  target->pendingKeyFrame = cleanPoint != 0;
  return true;
}

bool WmfMediaExtractor::shouldDropSample(const Track& track) const {
  if (mNextSyncTimeUs == Definitions::kTimeUnset) {
    return false;
  }
  if (track.pendingTimeUs < mNextSyncTimeUs) {
    return true;
  }
  // The first sample at or after the requested position must be a sync sample for video tracks.
  return track.mediaType == MediaType::kMediaTypeVideo && !track.pendingKeyFrame;
}

void WmfMediaExtractor::emitSample(Track& track) {
  if (mNextSyncTimeUs != Definitions::kTimeUnset) {
    if (shouldDropSample(track)) {
      track.pendingSample.Reset();
      return;
    }
    mNextSyncTimeUs = Definitions::kTimeUnset;
  }

  auto sample = track.pendingSample;
  track.pendingSample.Reset();
  const auto timeUs = track.pendingTimeUs;
  const auto keyFrame = track.pendingKeyFrame;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || buffer == nullptr) {
    return;
  }
  BYTE* data = nullptr;
  DWORD maxLength = 0;
  DWORD currentLength = 0;
  if (FAILED(buffer->Lock(&data, &maxLength, &currentLength))) {
    return;
  }

  auto flags = AVBufferFlags::kNone;
  if (keyFrame) {
    flags = AVBufferFlags::kKeyFrame;
  }
  if (track.output != nullptr && currentLength > 0) {
    // The sample owns a copy of the bytes of the source reader, which are only valid until it is
    // unlocked: the output is free to keep the sample for as long as it needs it.
    track.output->sample(AVSample(track.trackId, timeUs, toSampleFlags(flags),
        std::make_shared<AVBlockBuffer>(data, static_cast<uint32_t>(currentLength))));
  }
  buffer->Unlock();

  if (mCurrentPosition == Definitions::kPositionUnset) {
    mCurrentPosition = currentLength;
  } else {
    mCurrentPosition += currentLength;
  }
}

bool WmfMediaExtractor::seekTo(const int64_t timeUs, const SeekOption option) {
  if (!mReader) {
    return false;
  }

  PROPVARIANT position;
  ::PropVariantInit(&position);
  position.vt = VT_I8;
  position.hVal.QuadPart = timeUs * 10; // us to 100ns

  const auto result = mReader->SetCurrentPosition(GUID_NULL, position);
  ::PropVariantClear(&position);
  if (FAILED(result)) {
    MINILOG_ERROR("failed to seek to {}us: {}", timeUs, WmfMediaUtils::hresultToString(result));
    return false;
  }

  for (auto& track: mTracks) {
    track.pendingSample.Reset();
    track.endOfStream = false;
  }
  mNextSyncTimeUs = option == SeekOption::SEEK_TO_NEXT_SYNC
                      ? timeUs
                      : Definitions::kTimeUnset;
  mCurrentPosition = Definitions::kPositionUnset;
  return true;
}

void WmfMediaExtractor::clear() {
  mTracks.clear();
  // The reader is released before the input it reads through, so that no read can reach an input that
  // is being let go of.
  mReader.Reset();
  mInput.reset();
  mOutput = nullptr;
  mPrepared = false;
  mCurrentPosition = Definitions::kPositionUnset;
  mNextSyncTimeUs = Definitions::kTimeUnset;
}

coral::SeekPoints WmfMediaExtractor::getPlatformSeekPoints(const int64_t timeUs) const {
  // The source reader performs the seek on the sample reader itself, the reported byte position is
  // only informative, so the requested time is reported for both seek points.
  const auto position = mCurrentPosition != Definitions::kPositionUnset ? mCurrentPosition : 0;
  return coral::SeekPoints(coral::SeekPoint(timeUs < 0 ? 0 : timeUs, position));
}

} // namespace coral
