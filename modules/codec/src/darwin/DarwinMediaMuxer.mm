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

#if defined(__APPLE__)

#include "DarwinMediaMuxer.h"

#include "DarwinMediaUtils.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <minilog/log.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The synchronous asset API is deprecated on macOS 15 and iOS 16 in favour of the asynchronous load
// methods, but the asset writer itself is not: the pragma keeps this translation unit quiet when it
// is compiled on its own, which is what the build does globally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace coral {

namespace {

//!< The number of milliseconds a writer input may stay busy before the packet is dropped.
constexpr int MAX_READY_WAIT_MS = 10000;

//!< The number of seconds the writer may take to finalize the file.
constexpr int FINISH_WRITING_TIMEOUT_SECONDS = 30;

//!< The length of the NAL unit prefix a sample of the writer holds.
constexpr int NAL_LENGTH_SIZE = 4;

std::string describeError(NSError* error) {
  if (error == nil) {
    return "unknown error";
  }
  NSString* description = error.localizedDescription;
  if (description.length == 0) {
    return std::string("error ") + std::to_string(static_cast<long long>(error.code));
  }
  return std::string(description.UTF8String) + " (" +
         std::to_string(static_cast<long long>(error.code)) + ")";
}

void logError(NSError* error, const std::string& message) {
  darwin::logFailure(message, describeError(error));
}

/**
 * Converts the url of the caller into an NSURL.
 *
 * A url with a scheme, e.g. `file:///tmp/media.mp4`, is parsed as it is; anything else is a path of
 * the file system.
 */
NSURL* urlFromString(const std::string& url) {
  if (url.empty()) {
    return nil;
  }
  NSString* text = [NSString stringWithUTF8String:url.c_str()];
  if (text == nil) {
    return nil;
  }
  NSURL* parsed = [NSURL URLWithString:text];
  if (parsed != nil && parsed.scheme.length > 0) {
    return parsed;
  }
  return [NSURL fileURLWithPath:text];
}

/**
 * Returns the file type of the container the format selects.
 */
AVFileType fileTypeOf(const MuxerFormat format) {
  return format == MuxerFormat::kMuxerFormat3Gpp ? AVFileType3GPP : AVFileTypeMPEG4;
}

} // namespace

/**
 * The platform state of the muxer: the asset writer, and the input of every track.
 *
 * The Objective-C objects are strong members of the pimpl, which is compiled with ARC.
 */
struct DarwinMediaMuxer::Impl {
  /**
   * The state of a track which has been added to the writer.
   */
  struct Track {
    //!< The input of the writer that receives the packets of the track.
    __strong AVAssetWriterInput* input = nil;
    //!< The format the track was added with.
    std::shared_ptr<MediaFormat> format;
    //!< The format description of the track, which every sample buffer of the track refers to.
    CMFormatDescriptionRef description = nullptr;
    //!< The number of audio frames one compressed packet carries, 0 for a video track.
    int framesPerPacket = 0;
    //!< Whether the track contains video data.
    bool video = false;
    //!< The index the caller identifies the track with.
    int streamIndex = 0;
  };

  explicit Impl(std::atomic<Status>* status) : status(status) {}

  bool prepare(std::string newUrl, MuxerFormat newFormat);

  int addTrack(const std::shared_ptr<MediaFormat>& format);

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer);

  void start();

  void stop();

  void release();

private:
  /**
   * Marks every input as finished and waits until the writer wrote the file, then releases the writer
   * and the descriptions of the tracks.
   *
   * @return whether the writer completed the file.
   */
  bool finishWriting();

  /**
   * Lets go of the input and the format description of every track, and of the writer.
   */
  void releaseWriter();

  //!< The status of the muxer, which is owned by the muxer itself so that its base class reports it.
  std::atomic<Status>* status;

  //!< The url the muxer writes to, which is only what the diagnostics report.
  std::string url;
  MuxerFormat format = MuxerFormat::kMuxerFormatInvalid;
  //!< The writer of the container, null until {@link #prepare} succeeded.
  __strong AVAssetWriter* writer = nil;
  __strong NSURL* outputUrl = nil;
  std::vector<Track> tracks;

  //!< Whether the writer has been finalized, so that it is not finalized twice.
  bool finalized = false;
};

bool DarwinMediaMuxer::Impl::prepare(std::string newUrl, const MuxerFormat newFormat) {
  if (!DarwinMediaMuxer::isSupportedFormat(newFormat)) {
    MINILOG_ERROR("the platform does not support the muxer format: {}", convertToText(newFormat));
    return false;
  }
  if (newUrl.empty()) {
    MINILOG_ERROR("the output url is empty");
    return false;
  }
  NSURL* newOutputUrl = urlFromString(newUrl);
  if (newOutputUrl == nil) {
    MINILOG_ERROR("the output url is not a valid file url: {}", newUrl);
    return false;
  }

  // A preparation replaces the writer of a previous one, so that the tracks of the previous
  // preparation cannot reach the new file.
  release();

  // The asset writer refuses to overwrite a file that exists, so the file of a previous muxing of the
  // same url is removed first.
  if (newOutputUrl.isFileURL) {
    [[NSFileManager defaultManager] removeItemAtURL:newOutputUrl error:nil];
  }

  NSError* error = nil;
  AVAssetWriter* newWriter = [[AVAssetWriter alloc] initWithURL:newOutputUrl
                                                       fileType:fileTypeOf(newFormat)
                                                          error:&error];
  if (newWriter == nil) {
    logError(error, "the asset writer of " + newUrl + " could not be created");
    return false;
  }

  url = std::move(newUrl);
  format = newFormat;
  writer = newWriter;
  outputUrl = newOutputUrl;
  tracks.clear();
  finalized = false;
  *status = Status::kMuxerStatusPrepared;
  return true;
}

int DarwinMediaMuxer::Impl::addTrack(const std::shared_ptr<MediaFormat>& trackFormat) {
  if (writer == nil || trackFormat == nullptr) {
    return kInvalidTrack;
  }
  if (*status != Status::kMuxerStatusPrepared) {
    MINILOG_ERROR("the track must be added before the muxer is started");
    return kInvalidTrack;
  }

  CMFormatDescriptionRef description = darwin::createFormatDescription(trackFormat);
  if (description == nullptr) {
    MINILOG_ERROR("the format of the track is not supported: {}", trackFormat->toString());
    return kInvalidTrack;
  }

  AVMediaType mediaType = nil;
  if (trackFormat->isVideo()) {
    mediaType = AVMediaTypeVideo;
  } else if (trackFormat->isAudio()) {
    mediaType = AVMediaTypeAudio;
  }
  if (mediaType == nil) {
    MINILOG_ERROR("the media type of the track is not supported: {}", trackFormat->toString());
    CFRelease(description);
    return kInvalidTrack;
  }

  // The packets of the track are already encoded, so the input writes the samples as they are and
  // the format description declares their codec.
  AVAssetWriterInput* input = [[AVAssetWriterInput alloc] initWithMediaType:mediaType
                                                            outputSettings:nil
                                                         sourceFormatHint:description];
  input.expectsMediaDataInRealTime = NO;
  const int rotation = trackFormat->getRotation();
  if (rotation != 0) {
    input.transform = CGAffineTransformMakeRotation(
      static_cast<CGFloat>(rotation) * static_cast<CGFloat>(M_PI) / 180.0);
  }
  if (![writer canAddInput:input]) {
    logError(writer.error, "the input of the track " + trackFormat->toString() +
                           " could not be added to the writer");
    CFRelease(description);
    return kInvalidTrack;
  }
  [writer addInput:input];

  Track track;
  track.input = input;
  track.format = trackFormat;
  track.description = description;
  track.video = trackFormat->isVideo();
  track.streamIndex = static_cast<int>(tracks.size());
  if (!track.video) {
    // The duration of an audio sample is only known when the codec puts a fixed number of frames in
    // one packet; a video sample is timed with the frame rate of its track.
    track.framesPerPacket = darwin::defaultFramesPerPacket(trackFormat->getAudioFormatType());
  }
  tracks.emplace_back(std::move(track));
  return tracks.back().streamIndex;
}

void DarwinMediaMuxer::Impl::writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) {
  if (writer == nil || buffer == nullptr) {
    return;
  }
  if (*status != Status::kMuxerStatusStarted) {
    return;
  }
  if (buffer->isCodecConfig() || buffer->isEndOfStream()) {
    return;
  }
  const int streamIndex = buffer->streamIndex();
  if (streamIndex < 0 || static_cast<size_t>(streamIndex) >= tracks.size()) {
    MINILOG_ERROR("the stream index of the packet is invalid: {}", streamIndex);
    return;
  }

  Track& track = tracks[static_cast<size_t>(streamIndex)];
  if (track.input == nil || track.description == nullptr) {
    return;
  }
  const size_t size = buffer->size();
  const uint8_t* data = buffer->array();
  if (size == 0 || data == nullptr) {
    return;
  }

  // The writer expects the length prefixed form of the video stream, while the packets of the
  // framework carry the Annex B byte stream; an audio packet is written as it is.
  std::vector<uint8_t> normalized;
  size_t dataSize = size;
  if (track.video) {
    normalized = darwin::toLengthPrefixed(data, size, NAL_LENGTH_SIZE);
    if (normalized.empty()) {
      MINILOG_ERROR("the video sample of the track {} could not be converted, size: {}",
        streamIndex, size);
      return;
    }
    data = normalized.data();
    dataSize = normalized.size();
  }

  // The block buffer takes ownership of the memory and frees it with the allocator it was created
  // with, so the packet does not have to outlive the sample buffer.
  auto* memory = static_cast<uint8_t*>(malloc(dataSize));
  if (memory == nullptr) {
    MINILOG_ERROR("the sample of the track {} could not be allocated, size: {}", streamIndex,
      dataSize);
    return;
  }
  memcpy(memory, data, dataSize);

  CMBlockBufferRef block = nullptr;
  OSStatus result = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, memory, dataSize,
    kCFAllocatorMalloc, nullptr, 0, dataSize, kCMBlockBufferAssureMemoryNowFlag, &block);
  if (result != kCMBlockBufferNoErr || block == nullptr) {
    free(memory);
    darwin::logStatus(result, "the sample of the track " + std::to_string(streamIndex) +
                              " could not be created");
    return;
  }

  CMTime duration = kCMTimeInvalid;
  if (track.video) {
    const int frameRate = track.format != nullptr ? track.format->getFrameRate()
                                                  : MediaFormat::NO_VALUE;
    if (frameRate != MediaFormat::NO_VALUE && frameRate > 0) {
      duration = CMTimeMake(1, frameRate);
    }
  } else {
    const int sampleRate = track.format != nullptr ? track.format->getSampleRate() : 0;
    if (track.framesPerPacket > 0 && sampleRate > 0) {
      duration = CMTimeMake(track.framesPerPacket, sampleRate);
    }
  }

  CMSampleTimingInfo timing{};
  timing.duration = duration;
  timing.presentationTimeStamp = darwin::toCMTime(buffer->timeUs());
  timing.decodeTimeStamp = kCMTimeInvalid;
  size_t sampleSize = dataSize;
  CMSampleBufferRef sample = nullptr;
  result = CMSampleBufferCreateReady(kCFAllocatorDefault, block, track.description, 1, 1, &timing, 1,
    &sampleSize, &sample);
  CFRelease(block);
  if (result != noErr || sample == nullptr) {
    darwin::logStatus(result, "the sample of the track " + std::to_string(streamIndex) +
                              " could not be described");
    return;
  }

  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
  if (attachments != nullptr && CFArrayGetCount(attachments) > 0) {
    auto attachment = const_cast<CFMutableDictionaryRef>(
      static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0)));
    if (attachment != nullptr) {
      // An audio packet is always a sync sample; a video packet only when the encoder said so.
      const bool sync = !track.video || buffer->isKeyFrame();
      CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync,
        sync ? kCFBooleanFalse : kCFBooleanTrue);
    }
  }

  // The writer only accepts a sample when the input is ready for it, and it stays busy for as long
  // as it writes; the wait is bounded so that a writer which stopped accepting samples cannot hang
  // the caller.
  int waitedMs = 0;
  while (!track.input.isReadyForMoreMediaData && waitedMs < MAX_READY_WAIT_MS) {
    if (writer.status == AVAssetWriterStatusFailed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++waitedMs;
  }
  if (!track.input.isReadyForMoreMediaData) {
    MINILOG_ERROR("the input of the track {} was not ready for more data after {}ms", streamIndex,
      waitedMs);
    CFRelease(sample);
    if (writer.status == AVAssetWriterStatusFailed) {
      logError(writer.error, "the asset writer of the media " + url + " failed");
      *status = Status::kMuxerStatusError;
    }
    return;
  }

  if (![track.input appendSampleBuffer:sample]) {
    logError(writer.error, "the sample of the track " + std::to_string(streamIndex) +
                           " could not be written to " + url);
  }
  CFRelease(sample);

  if (writer.status == AVAssetWriterStatusFailed) {
    logError(writer.error, "the asset writer of the media " + url + " failed");
    *status = Status::kMuxerStatusError;
  }
}

void DarwinMediaMuxer::Impl::start() {
  if (*status == Status::kMuxerStatusStarted) {
    return;
  }
  if (writer == nil) {
    MINILOG_ERROR("the muxer is not prepared");
    *status = Status::kMuxerStatusError;
    return;
  }
  if (![writer startWriting]) {
    logError(writer.error, "the asset writer of the media " + url + " could not start writing");
    *status = Status::kMuxerStatusError;
    return;
  }
  // Every packet of the framework is expressed relative to the start of the media, which is the
  // source time of the session of the writer.
  [writer startSessionAtSourceTime:kCMTimeZero];
  *status = Status::kMuxerStatusStarted;
}

void DarwinMediaMuxer::Impl::stop() {
  if (*status != Status::kMuxerStatusStarted) {
    return;
  }
  const bool completed = finishWriting();
  *status = completed ? Status::kMuxerStatusStopped : Status::kMuxerStatusError;
}

bool DarwinMediaMuxer::Impl::finishWriting() {
  if (writer == nil || finalized) {
    return finalized;
  }
  for (auto& track : tracks) {
    if (track.input != nil) {
      [track.input markAsFinished];
    }
  }

  // Finishing the file is asynchronous: the writer calls the handler once the container is complete.
  __block BOOL completed = NO;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  [writer finishWritingWithCompletionHandler:^{
    completed = YES;
    dispatch_semaphore_signal(semaphore);
  }];
  const auto timeout = dispatch_time(DISPATCH_TIME_NOW,
    static_cast<int64_t>(FINISH_WRITING_TIMEOUT_SECONDS) * NSEC_PER_SEC);
  const bool signaled = dispatch_semaphore_wait(semaphore, timeout) == 0;
  finalized = YES;

  const bool finished = signaled && completed && writer.status == AVAssetWriterStatusCompleted;
  if (!finished) {
    logError(writer.error, "the asset writer of the media " + url + " could not finish writing");
  }
  // The writer and the descriptions of the tracks are not needed once the file is complete.
  releaseWriter();
  return finished;
}

void DarwinMediaMuxer::Impl::releaseWriter() {
  for (auto& track : tracks) {
    track.input = nil;
    if (track.description != nullptr) {
      CFRelease(track.description);
      track.description = nullptr;
    }
  }
  writer = nil;
}

void DarwinMediaMuxer::Impl::release() {
  if (*status == Status::kMuxerStatusStarted) {
    stop();
  } else {
    releaseWriter();
  }
  tracks.clear();
  writer = nil;
  outputUrl = nil;
  finalized = false;
  format = MuxerFormat::kMuxerFormatInvalid;
  *status = Status::kMuxerStatusIdle;
}

bool DarwinMediaMuxer::isSupportedFormat(const MuxerFormat format) {
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4:
    case MuxerFormat::kMuxerFormat3Gpp: {
      return true;
    }
    default: {
      return false;
    }
  }
}

DarwinMediaMuxer::DarwinMediaMuxer()
  : mImpl(std::make_unique<Impl>(&mStatus)) {
}

DarwinMediaMuxer::~DarwinMediaMuxer() {
  release();
}

bool DarwinMediaMuxer::prepare(std::string url, const MuxerFormat format) {
  @autoreleasepool {
    return mImpl->prepare(std::move(url), format);
  }
}

int DarwinMediaMuxer::addTrack(const std::shared_ptr<MediaFormat>& format) {
  @autoreleasepool {
    return mImpl->addTrack(format);
  }
}

void DarwinMediaMuxer::writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) {
  @autoreleasepool {
    mImpl->writeFrame(buffer);
  }
}

void DarwinMediaMuxer::start() {
  @autoreleasepool {
    mImpl->start();
  }
}

void DarwinMediaMuxer::stop() {
  @autoreleasepool {
    mImpl->stop();
  }
}

void DarwinMediaMuxer::release() {
  @autoreleasepool {
    mImpl->release();
  }
}

} // namespace coral

#pragma clang diagnostic pop

#endif // defined(__APPLE__)
