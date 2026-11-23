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

#include "DarwinMediaExtractor.h"

#include "DarwinMediaUtils.h"

#include <media/Definitions.h>
#include <media/Types.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <minilog/log.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The synchronous asset API (`duration`, `tracksWithMediaType:` and the properties of a track) is
// deprecated on macOS 15 and iOS 16 in favour of the asynchronous load methods, but it is still
// available on both platforms, and it is what a demuxer that resolves a track before it starts
// reading needs. The build disables the deprecation warnings globally; the pragma keeps this
// translation unit quiet when it is compiled on its own.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace coral {

namespace {

//!< The number of sample buffers that may carry no media data before a track is treated as ended.
constexpr int MAX_MARKER_SKIPS = 4096;

//!< The number of read rounds before the read of the extractor returns without any progress.
constexpr int MAX_READ_RETRY = 4096;

//!< The size of the largest video sample of the platform extractor.
constexpr int MAX_VIDEO_INPUT_SIZE = 4 * 1024 * 1024;

//!< The size of the largest audio sample of the platform extractor.
constexpr int MAX_AUDIO_INPUT_SIZE = 1024 * 1024;

//!< The granularity the rotation of a track is reported with.
constexpr int ROTATION_STEP_DEGREES = 90;

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
 * A url with a scheme, e.g. `file:///tmp/media.mp4` or `https://example.com/media.mp4`, is parsed as
 * it is; anything else is a path of the file system.
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
 * Returns the rotation of the track in degrees, i.e. one of 0, 90, 180 and 270.
 *
 * The preferred transform of a track expresses the rotation as the angle between the positive x axis
 * of the media and the one of the presentation, so the angle of the transform is the rotation.
 */
int rotationOf(const CGAffineTransform& transform) {
  const double degrees = std::atan2(transform.b, transform.a) * 180.0 / M_PI;
  auto rotation = static_cast<int>(std::lround(degrees / ROTATION_STEP_DEGREES)) *
                  ROTATION_STEP_DEGREES;
  rotation %= 360;
  if (rotation < 0) {
    rotation += 360;
  }
  return rotation;
}

/**
 * Returns the mime type of the container of the media, as the format of a track reports it.
 *
 * The extension of the url is what tells MP3, WAV and FLAC apart, since AVFoundation reads them with
 * the same asset reader; every other supported extension is an ISO base media file container.
 */
std::string containerMimeOf(NSString* pathExtension, const bool hasVideo) {
  const std::string fallback = hasVideo ? MimeTypes::VIDEO_MP4 : MimeTypes::AUDIO_MP4;
  NSString* extension = pathExtension.lowercaseString;
  if (extension.length == 0) {
    return fallback;
  }
  if ([extension isEqualToString:@"mp3"]) {
    return MimeTypes::AUDIO_MPEG;
  }
  if ([extension isEqualToString:@"wav"]) {
    return MimeTypes::AUDIO_WAV;
  }
  if ([extension isEqualToString:@"flac"]) {
    return MimeTypes::AUDIO_FLAC;
  }
  if ([extension isEqualToString:@"mp4"] || [extension isEqualToString:@"m4v"] ||
      [extension isEqualToString:@"m4a"] || [extension isEqualToString:@"mov"] ||
      [extension isEqualToString:@"3gp"] || [extension isEqualToString:@"aac"]) {
    return hasVideo ? MimeTypes::VIDEO_MP4 : MimeTypes::AUDIO_MP4;
  }
  return fallback;
}

/**
 * Indicates whether the media data of a sample buffer is a sync sample.
 *
 * CoreMedia reports the opposite: a sample is a sync sample unless its attachments declare it as not
 * sync. An audio sample is always a sync sample.
 */
bool isSyncSample(CMSampleBufferRef sample) {
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (attachments == nullptr || CFArrayGetCount(attachments) == 0) {
    return true;
  }
  CFDictionaryRef attachment =
    static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
  if (attachment == nullptr) {
    return true;
  }
  CFTypeRef value = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync);
  if (value == nullptr || CFGetTypeID(value) != CFBooleanGetTypeID()) {
    return true;
  }
  return !CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

/**
 * Returns the presentation timestamp of one sample of a sample buffer.
 *
 * The asset reader coalesces the compressed audio packets of a track into one sample buffer, which
 * then describes the timing of its samples in one of two ways: one entry that describes the whole
 * range, in which case the entry of a sample is the base time plus its offset, or one entry per
 * sample.
 *
 * @param basePts       the timestamp of the sample buffer.
 * @param stepDuration  the duration of one sample, or an invalid time when it is not known.
 * @param timings       the timing entries of the sample buffer, or null when there are none.
 * @param timingEntries the number of valid entries of `timings`.
 * @param sampleCount   the number of samples the sample buffer carries.
 * @param index         the index of the sample.
 * @param fallbackUs    the timestamp reported when the sample buffer does not describe one.
 */
int64_t sampleTimeUs(const CMTime basePts, const CMTime stepDuration,
  const CMSampleTimingInfo* timings, const CMItemCount timingEntries, const CMItemCount sampleCount,
  const CMItemCount index, const int64_t fallbackUs) {
  CMTime time = kCMTimeInvalid;
  if (timings != nullptr && timingEntries == sampleCount &&
      CMTIME_IS_VALID(timings[index].presentationTimeStamp)) {
    time = timings[index].presentationTimeStamp;
  } else if (CMTIME_IS_VALID(basePts)) {
    if (CMTIME_IS_VALID(stepDuration) && index > 0) {
      time = CMTimeAdd(basePts, CMTimeMultiply(stepDuration, static_cast<int32_t>(index)));
    } else {
      time = basePts;
    }
  }
  const auto timeUs = darwin::toTimeUs(time);
  return timeUs == Definitions::kTimeUnset ? fallbackUs : timeUs;
}

} // namespace

/**
 * The platform state of the extractor: the asset, the reader, and the tracks with the packets which
 * have been demuxed but not delivered yet.
 *
 * The Objective-C objects are strong members of the pimpl, which is compiled with ARC.
 */
struct DarwinMediaExtractor::Impl {
  /**
   * One compressed packet of a track, waiting to be delivered to its track output.
   */
  struct Packet {
    int64_t timeUs = Definitions::kTimeUnset;
    bool keyFrame = false;
    std::vector<uint8_t> data;
  };

  /**
   * The state of a track that the extractor demuxes.
   */
  struct Track {
    //!< The track id reported to the caller.
    int trackId = Definitions::kInvalidValue;
    //!< The audio/video media type of the track.
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    //!< Whether the track contains video data.
    bool video = false;
    /**
     * Whether a sample buffer of the track is split into one packet per declared sample.
     *
     * A compressed audio packet and a video frame are the unit the decoders consume, and an asset
     * reader buffer can carry several of them. An uncompressed audio buffer is kept as one packet:
     * its samples are single pcm frames, and splitting them would turn one buffer into thousands of
     * packets.
     */
    bool splitSamples = true;
    //!< Whether the video samples must be converted to the Annex B byte stream.
    bool convertToAnnexB = false;
    //!< The length of a NAL unit prefix of the video stream.
    int nalLengthSize = 4;
    //!< The track of the asset, which the reader asks for the samples of the track.
    __strong AVAssetTrack* assetTrack = nil;
    //!< The output of the current reader that reads the track, null after a seek or at the end.
    __strong AVAssetReaderTrackOutput* readerOutput = nil;
    //!< The output that receives the format and the samples of the track.
    std::shared_ptr<AVTrackOutput> output;
    //!< The packets which have been demuxed but not delivered yet, ordered by decode time.
    std::deque<Packet> queue;
    //!< Whether the reader reached the end of the track.
    bool ended = false;
  };

  explicit Impl(std::string url) : url(std::move(url)) {}

  //!< The url the extractor was created for, which is only what the diagnostics report.
  std::string url;
  //!< The mime type of the container, which the formats of the tracks report.
  std::string containerMime;
  //!< The asset of the media.
  __strong AVURLAsset* asset = nil;
  //!< The reader of the current read position, null until {@link #init} or after a failed seek.
  __strong AVAssetReader* reader = nil;
  std::vector<Track> tracks;
  //!< The output of the reader that receives the tracks and the seek map. Owned by the reader, which
  //!< keeps it alive for as long as it drives this extractor, and never handed out again.
  AVExtractorOutput* extractorOutput = nullptr;

  int64_t durationUs = Definitions::kTimeUnset;
  int64_t currentPosition = Definitions::kPositionUnset;
  int64_t nextSyncTimeUs = Definitions::kTimeUnset;
  //!< Whether the media has a video track, i.e. whether a pending seek waits for a sync sample.
  bool hasVideoTrack = false;
  bool prepared = false;

  /**
   * Opens the asset of the url, and reads its duration, its tracks and the mime type of its
   * container.
   */
  bool open();

  /**
   * Creates the reader and reports the tracks and their formats to the output.
   *
   * @return whether at least one track was added and the reader started.
   */
  bool init(AVExtractorOutput& trackOutput);

  PlatformResult read(PositionHolder& seekPosition);

  bool seekTo(int64_t timeUs, SeekOption option);

  void clear();

private:
  /**
   * Creates a reader of the asset and adds an output of every track to it.
   *
   * An output can only be added to one reader, so a seek replaces the outputs of the tracks together
   * with the reader.
   *
   * @param startTimeUs the time the reader starts at, or a negative value to start at the beginning.
   */
  bool createReader(int64_t startTimeUs);

  /**
   * Cancels the reader and lets go of it together with the outputs of the tracks.
   */
  void stopReader();

  /**
   * Reads the next sample buffer of the track and queues its packets.
   *
   * @return whether packets were queued; when false, the track has ended.
   */
  bool pull(Track& track);

  /**
   * Splits the sample buffer into one packet per sample and queues them.
   */
  void enqueueSamples(Track& track, CMSampleBufferRef sample, CMBlockBufferRef block,
    CMItemCount sampleCount);

  /**
   * Appends one packet to the queue of the track, in the form the decoders consume.
   */
  void appendPacket(Track& track, int64_t timeUs, bool keyFrame, const uint8_t* data, size_t size);

  /**
   * Delivers the packet at the head of the queue of the track.
   *
   * @return whether a packet was delivered; false when it was dropped by a pending seek.
   */
  bool deliver(Track& track);

  /**
   * Indicates whether the packet should be dropped because of a pending SEEK_TO_NEXT_SYNC request.
   */
  [[nodiscard]] bool shouldDrop(const Track& track, const Packet& packet) const;
};

bool DarwinMediaExtractor::Impl::open() {
  NSURL* assetUrl = urlFromString(this->url);
  if (assetUrl == nil) {
    MINILOG_ERROR("the url is not a valid media url: {}", this->url);
    return false;
  }
  AVURLAsset* newAsset = [[AVURLAsset alloc] initWithURL:assetUrl
                                                 options:@{
                                                   AVURLAssetPreferPreciseDurationAndTimingKey: @YES
                                                 }];
  if (newAsset == nil || !newAsset.readable) {
    MINILOG_ERROR("the media is not readable: {}", this->url);
    return false;
  }
  const bool hasVideo = [newAsset tracksWithMediaType:AVMediaTypeVideo].count > 0;
  const bool hasAudio = [newAsset tracksWithMediaType:AVMediaTypeAudio].count > 0;
  if (!hasVideo && !hasAudio) {
    MINILOG_ERROR("the media has no audio and no video track: {}", this->url);
    return false;
  }
  asset = newAsset;
  durationUs = darwin::toTimeUs(newAsset.duration);
  containerMime = containerMimeOf(assetUrl.pathExtension, hasVideo);
  return true;
}

bool DarwinMediaExtractor::Impl::init(AVExtractorOutput& trackOutput) {
  extractorOutput = &trackOutput;
  tracks.clear();
  hasVideoTrack = false;
  prepared = false;
  if (asset == nil) {
    return false;
  }

  int nextTrackId = 0;
  const auto addTracks = [&](AVMediaType mediaType, const MediaType type, const bool video) {
    NSArray<AVAssetTrack*>* assetTracks = [asset tracksWithMediaType:mediaType];
    for (AVAssetTrack* assetTrack in assetTracks) {
      NSArray* descriptions = assetTrack.formatDescriptions;
      if (descriptions.count == 0) {
        MINILOG_DEBUG("skip the track {}: the asset declares no format description", nextTrackId);
        continue;
      }
      const auto description =
        (__bridge CMFormatDescriptionRef)[descriptions objectAtIndex:0];
      if (description == nullptr) {
        continue;
      }
      auto format = darwin::createMediaFormat(description, type, containerMime, nextTrackId);
      if (format == nullptr) {
        MINILOG_DEBUG("skip the track of the media {}: {}", url,
          darwin::formatDescriptionSummary(description));
        continue;
      }

      // The properties of the track are more accurate than the ones of the format description, so
      // they replace the values the container declared.
      if (video) {
        // The nominal frame rate of an audio track is the rate of its packets rather than the rate
        // of a frame, so it only describes a video track.
        const float nominalFrameRate = assetTrack.nominalFrameRate;
        if (nominalFrameRate > 0) {
          format->setFrameRate(static_cast<int>(std::lround(nominalFrameRate)));
        }
      }
      const float estimatedDataRate = assetTrack.estimatedDataRate;
      if (estimatedDataRate > 0) {
        format->setAverageBitrate(static_cast<int>(std::lround(estimatedDataRate)));
      }
      NSString* language = assetTrack.languageCode;
      if (language.length > 0) {
        format->setLanguage(language.UTF8String);
      }
      format->setRotation(rotationOf(assetTrack.preferredTransform));
      if (format->getMaxInputSize() == MediaFormat::NO_VALUE || format->getMaxInputSize() <= 0) {
        format->setMaxInputSize(video ? MAX_VIDEO_INPUT_SIZE : MAX_AUDIO_INPUT_SIZE);
      }

      auto output = extractorOutput->track(nextTrackId, type, AVTrackOutput::emptyConsumer());
      if (output == nullptr) {
        continue;
      }

      // Reports the format to the caller before the samples are delivered.
      output->format(format);

      Track track;
      track.trackId = nextTrackId;
      track.mediaType = type;
      track.video = video;
      track.assetTrack = assetTrack;
      track.output = std::move(output);
      track.splitSamples = video ||
        format->getAudioFormatType() != AudioFormatType::kAudioFormatTypeLinearPCM;
      if (video) {
        const auto codecType = format->getVideoCodecType();
        track.convertToAnnexB = codecType == VideoCodecType::kVideoCodecTypeH264 ||
                                codecType == VideoCodecType::kVideoCodecTypeHevc;
        track.nalLengthSize = darwin::nalLengthSizeOf(description);
        hasVideoTrack = true;
      }
      tracks.emplace_back(std::move(track));
      ++nextTrackId;
    }
  };

  addTracks(AVMediaTypeAudio, MediaType::kMediaTypeAudio, false);
  addTracks(AVMediaTypeVideo, MediaType::kMediaTypeVideo, true);
  if (tracks.empty()) {
    return false;
  }
  return createReader(-1);
}

bool DarwinMediaExtractor::Impl::createReader(const int64_t startTimeUs) {
  stopReader();
  if (asset == nil) {
    return false;
  }

  NSError* error = nil;
  AVAssetReader* newReader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
  if (newReader == nil) {
    logError(error, "the asset reader of the media " + url + " could not be created");
    return false;
  }
  if (startTimeUs > 0) {
    // The time range of a reader can only be set before it starts reading, which is why a seek
    // recreates the reader instead of repositioning it.
    newReader.timeRange = CMTimeRangeMake(
      CMTimeMake(startTimeUs, Definitions::kMicrosPerSecond), kCMTimePositiveInfinity);
  }

  for (auto& track : tracks) {
    if (track.assetTrack == nil) {
      continue;
    }
    AVAssetReaderTrackOutput* readerOutput =
      [[AVAssetReaderTrackOutput alloc] initWithTrack:track.assetTrack outputSettings:nil];
    // The samples are copied into the packet buffer of the extractor, so the reader does not have to
    // copy them again.
    readerOutput.alwaysCopiesSampleData = NO;
    // An output can only be added to one reader, which is why the outputs are recreated together
    // with the reader by a seek.
    [newReader addOutput:readerOutput];
    track.readerOutput = readerOutput;
    track.ended = false;
    track.queue.clear();
  }

  if (![newReader startReading]) {
    logError(newReader.error, "the asset reader of the media " + url + " could not start reading");
    for (auto& track : tracks) {
      track.readerOutput = nil;
    }
    return false;
  }
  reader = newReader;
  return true;
}

void DarwinMediaExtractor::Impl::stopReader() {
  if (reader != nil) {
    [reader cancelReading];
    reader = nil;
  }
  for (auto& track : tracks) {
    track.readerOutput = nil;
    track.ended = false;
    track.queue.clear();
  }
}

AVMediaExtractor::PlatformResult DarwinMediaExtractor::Impl::read(PositionHolder& seekPosition) {
  static_cast<void>(seekPosition);
  if (!prepared || extractorOutput == nullptr || reader == nil) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  for (int retry = 0; retry < MAX_READ_RETRY; ++retry) {
    // Deliver the queued packet with the earliest presentation timestamp first, so the packets of
    // the different tracks stay interleaved.
    Track* earliest = nullptr;
    for (auto& track : tracks) {
      if (track.queue.empty()) {
        continue;
      }
      if (earliest == nullptr || track.queue.front().timeUs < earliest->queue.front().timeUs) {
        earliest = &track;
      }
    }
    if (earliest != nullptr) {
      if (deliver(*earliest)) {
        return PlatformResult::RESULT_CONTINUE;
      }
      // The packet was dropped by a pending seek, so the next packet of the queue is looked at
      // instead of reporting a packet that the caller must not receive.
      continue;
    }

    bool alive = false;
    bool pulled = false;
    for (auto& track : tracks) {
      if (track.readerOutput == nil || track.ended) {
        continue;
      }
      alive = true;
      if (pull(track)) {
        pulled = true;
      }
    }
    if (pulled) {
      // The next round delivers the packets that were just queued.
      continue;
    }
    if (!alive) {
      return PlatformResult::RESULT_END_OF_INPUT;
    }
  }
  return PlatformResult::RESULT_CONTINUE;
}

bool DarwinMediaExtractor::Impl::pull(Track& track) {
  for (int attempt = 0; attempt < MAX_MARKER_SKIPS; ++attempt) {
    if (reader == nil) {
      track.ended = true;
      return false;
    }
    CMSampleBufferRef sample = [track.readerOutput copyNextSampleBuffer];
    if (sample == nullptr) {
      if (reader.status == AVAssetReaderStatusFailed) {
        logError(reader.error, "the asset reader of the media " + url + " failed");
      }
      track.ended = true;
      return false;
    }

    const CMItemCount sampleCount = CMSampleBufferGetNumSamples(sample);
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    // The reader can hand out sample buffers that only carry a marker, e.g. the end of a track or a
    // format change: `CMBlockBufferGetDataLength` must not be called on the null block of one.
    if (sampleCount <= 0 || block == nullptr || CMBlockBufferGetDataLength(block) == 0) {
      CFRelease(sample);
      continue;
    }
    enqueueSamples(track, sample, block, sampleCount);
    CFRelease(sample);
    if (!track.queue.empty()) {
      return true;
    }
  }
  MINILOG_DEBUG("the track {} produced no media data in {} sample buffers", track.trackId,
    MAX_MARKER_SKIPS);
  track.ended = true;
  return false;
}

void DarwinMediaExtractor::Impl::enqueueSamples(Track& track, CMSampleBufferRef sample,
    CMBlockBufferRef block, const CMItemCount sampleCount) {
  const size_t blockLength = CMBlockBufferGetDataLength(block);
  if (blockLength == 0) {
    return;
  }
  std::vector<uint8_t> bytes(blockLength);
  if (CMBlockBufferCopyDataBytes(block, 0, blockLength, bytes.data()) != kCMBlockBufferNoErr) {
    MINILOG_ERROR("the data of a sample of the track {} could not be copied", track.trackId);
    return;
  }

  const auto elementCount = static_cast<size_t>(sampleCount);
  auto* sizes = static_cast<size_t*>(malloc(sizeof(size_t) * elementCount));
  auto* timings = static_cast<CMSampleTimingInfo*>(
    malloc(sizeof(CMSampleTimingInfo) * elementCount));
  CMItemCount sizeEntries = 0;
  CMItemCount timingEntries = 0;
  OSStatus sizeStatus = noErr;
  OSStatus timingStatus = noErr;
  if (sizes != nullptr) {
    sizeStatus = CMSampleBufferGetSampleSizeArray(sample, sampleCount, sizes, &sizeEntries);
  }
  if (timings != nullptr) {
    timingStatus = CMSampleBufferGetSampleTimingInfoArray(sample, sampleCount, timings,
      &timingEntries);
  }
  if (timingStatus != noErr) {
    timingEntries = 0;
  }

  const bool keyFrame = track.video ? isSyncSample(sample) : true;
  CMTime basePts = CMSampleBufferGetPresentationTimeStamp(sample);
  if (!CMTIME_IS_VALID(basePts) && timings != nullptr && timingEntries > 0) {
    basePts = timings[0].presentationTimeStamp;
  }
  const CMTime stepDuration = timings != nullptr && timingEntries > 0
                                ? timings[0].duration
                                : kCMTimeInvalid;
  int64_t fallbackUs = darwin::toTimeUs(basePts);
  if (fallbackUs == Definitions::kTimeUnset) {
    fallbackUs = track.queue.empty() ? 0 : track.queue.back().timeUs;
  }

  // The sizes of the samples of the block are only usable when the sample buffer declares one for
  // each of them, and when they add up to the block.
  bool split = track.splitSamples && sizes != nullptr && sizeStatus == noErr &&
               sizeEntries == sampleCount;
  if (split) {
    size_t total = 0;
    for (CMItemCount index = 0; index < sampleCount; ++index) {
      total += sizes[index];
    }
    split = total == blockLength;
  }

  if (split) {
    size_t offset = 0;
    for (CMItemCount index = 0; index < sampleCount; ++index) {
      const auto timeUs = sampleTimeUs(basePts, stepDuration, timings, timingEntries, sampleCount,
        index, fallbackUs);
      appendPacket(track, timeUs, keyFrame, bytes.data() + offset, sizes[index]);
      offset += sizes[index];
    }
  } else {
    // The sample buffer does not describe its samples: it is kept as one packet at its own
    // timestamp rather than being dropped.
    const auto timeUs = sampleTimeUs(basePts, stepDuration, timings, timingEntries, 1, 0,
      fallbackUs);
    appendPacket(track, timeUs, keyFrame, bytes.data(), bytes.size());
  }

  free(sizes);
  free(timings);
}

void DarwinMediaExtractor::Impl::appendPacket(Track& track, const int64_t timeUs,
    const bool keyFrame, const uint8_t* data, const size_t size) {
  if (data == nullptr || size == 0 || size > static_cast<size_t>(INT32_MAX)) {
    return;
  }
  Packet packet;
  packet.timeUs = timeUs;
  packet.keyFrame = keyFrame;
  if (track.convertToAnnexB) {
    // The asset reader hands out the length prefixed form of the video stream, while the decoders of
    // the framework consume the Annex B byte stream.
    auto converted = darwin::toAnnexB(data, size, track.nalLengthSize);
    if (converted.empty()) {
      packet.data.assign(data, data + size);
    } else {
      packet.data = std::move(converted);
    }
  } else {
    packet.data.assign(data, data + size);
  }
  track.queue.emplace_back(std::move(packet));
}

bool DarwinMediaExtractor::Impl::shouldDrop(const Track& track, const Packet& packet) const {
  if (nextSyncTimeUs == Definitions::kTimeUnset) {
    return false;
  }
  if (packet.timeUs < nextSyncTimeUs) {
    return true;
  }
  // The first packet at or after the requested position must be a sync sample of a video track.
  return track.video && !packet.keyFrame;
}

bool DarwinMediaExtractor::Impl::deliver(Track& track) {
  if (track.queue.empty()) {
    return false;
  }
  Packet packet = std::move(track.queue.front());
  track.queue.pop_front();
  if (shouldDrop(track, packet)) {
    return false;
  }

  // A pending seek is fulfilled by the sync sample of the video track it waits for: the asset reader
  // rewinds the video track to the sync sample at or before the requested position, so the packets
  // that follow are not the ones the seek asked for until the next sync sample is reached. A
  // container without a video track has nothing to wait for.
  if (nextSyncTimeUs != Definitions::kTimeUnset &&
      (!hasVideoTrack || (track.video && packet.keyFrame))) {
    nextSyncTimeUs = Definitions::kTimeUnset;
  }

  if (track.output != nullptr && !packet.data.empty()) {
    // The packet owns a copy of the bytes of the sample buffer, which is released by the caller: the
    // output is free to keep the packet for as long as it needs it.
    track.output->sample(AVSample(track.trackId, packet.timeUs,
      toSampleFlags(packet.keyFrame ? AVBufferFlags::kKeyFrame : AVBufferFlags::kNone),
      std::make_shared<AVBlockBuffer>(packet.data.data(),
        static_cast<uint32_t>(packet.data.size()))));
  }

  if (currentPosition == Definitions::kPositionUnset) {
    currentPosition = static_cast<int64_t>(packet.data.size());
  } else {
    currentPosition += static_cast<int64_t>(packet.data.size());
  }
  return true;
}

bool DarwinMediaExtractor::Impl::seekTo(const int64_t timeUs, const SeekOption option) {
  if (asset == nil || tracks.empty()) {
    return false;
  }
  stopReader();
  nextSyncTimeUs = option == SeekOption::SEEK_TO_NEXT_SYNC ? timeUs : Definitions::kTimeUnset;
  currentPosition = Definitions::kPositionUnset;
  return createReader(timeUs < 0 ? 0 : timeUs);
}

void DarwinMediaExtractor::Impl::clear() {
  stopReader();
  tracks.clear();
  extractorOutput = nullptr;
  prepared = false;
  currentPosition = Definitions::kPositionUnset;
  nextSyncTimeUs = Definitions::kTimeUnset;
}

DarwinMediaExtractor::DarwinMediaExtractor(std::string url)
  : mImpl(std::make_unique<Impl>(std::move(url))) {
}

DarwinMediaExtractor::~DarwinMediaExtractor() {
  clear();
}

std::shared_ptr<DarwinMediaExtractor> DarwinMediaExtractor::createWithUrl(const std::string& url) {
  if (url.empty()) {
    return nullptr;
  }
  @autoreleasepool {
    auto extractor = std::shared_ptr<DarwinMediaExtractor>(new DarwinMediaExtractor(url));
    if (!extractor->mImpl->open()) {
      return nullptr;
    }
    return extractor;
  }
}

int DarwinMediaExtractor::sniff(AVExtractorInput& input) {
  static_cast<void>(input);
  // The media was opened by the url this extractor was created with, so it recognizes the container
  // it was created for. The input of the reader is not the media it demuxes.
  return AVExtractorsFactory::kSniffResultPossible;
}

void DarwinMediaExtractor::init(AVExtractorOutput& output) {
  @autoreleasepool {
    const bool prepared = mImpl->init(output);
    // The reader has to be told that no more tracks will follow, even when the container declared
    // none that the platform can read.
    output.endTracks();
    if (prepared) {
      mImpl->prepared = true;
      // The extractor is the seek map of the media it reads, and it is owned by a shared pointer, so
      // the output can keep it alive for as long as it refers to it.
      output.seekMap(shared_from_this());
    }
  }
}

AVMediaExtractor::PlatformResult DarwinMediaExtractor::read(PositionHolder& seekPosition) {
  @autoreleasepool {
    return mImpl->read(seekPosition);
  }
}

bool DarwinMediaExtractor::seekTo(const int64_t timeUs, const SeekOption option) {
  @autoreleasepool {
    return mImpl->seekTo(timeUs, option);
  }
}

void DarwinMediaExtractor::clear() {
  @autoreleasepool {
    mImpl->clear();
  }
}

int64_t DarwinMediaExtractor::getDurationUs() const {
  return mImpl->durationUs;
}

int64_t DarwinMediaExtractor::getReadPosition() const {
  return mImpl->currentPosition;
}

bool DarwinMediaExtractor::isSeekable() const {
  // An asset that was opened can be read again at another time; the reader resolves the position of
  // the time itself.
  return mImpl->asset != nil;
}

coral::SeekPoints DarwinMediaExtractor::getPlatformSeekPoints(const int64_t timeUs) const {
  // The asset reader performs the seek on the asset itself, the reported byte position is only
  // informative, so the requested time is reported for both seek points.
  const auto position = mImpl->currentPosition != Definitions::kPositionUnset
                          ? mImpl->currentPosition
                          : 0;
  return coral::SeekPoints(coral::SeekPoint(timeUs < 0 ? 0 : timeUs, position));
}

} // namespace coral

#pragma clang diagnostic pop

#endif // defined(__APPLE__)
