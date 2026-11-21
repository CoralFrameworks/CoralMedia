/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_CODEC_EXTRACTOR_H
#define CORAL_CODEC_EXTRACTOR_H

#include <codec/CodecTypes.h>

#include <extractor/Extractor.h>
#include <extractor/ExtractorInput.h>

#include <media/Buffer.h>
#include <media/Format.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * An AVMediaExtractor is an interface to extract audio or video packet data.
 *
 * <p>It is an {@link AVExtractor}: the extractor of a platform, e.g. the source reader of the media
 * foundation on Windows or {@code android.media.MediaExtractor}, is driven by the reader of the
 * extractor module exactly as a built-in extractor is, and what it demuxes reaches the caller as the
 * {@link AVSample} of that module rather than as the raw bytes of a buffer. What remains specific to
 * the platforms is the way they are written internally, i.e. the three valued read and the seek map of
 * the platform shape, which the reader never calls:
 *
 * <ul>
 *   <li>{@link #read(AVExtractorInput&)} is the interface of the extractor module, and it bridges to
 *       {@link #read(PositionHolder&)};</li>
 *   <li>{@link #getSeekPoints(int64_t, SeekPoint&, SeekPoint&)} is the seek map of the extractor
 *       module, and it bridges to {@link #getPlatformSeekPoints(int64_t)};</li>
 *   <li>{@link #init(AVExtractorOutput&)}, {@link #seek(long, int64_t)} and the tracks are the
 *       interface of the extractor module, the platforms only have to implement them.</li>
 * </ul>
 *
 * <p>The extractor hands itself out as the seek map of the media it reads, so it is shared rather
 * than owned by its caller alone: see {@link ExtractorFactory}.
 */
class AVMediaExtractor : public AVExtractor,
                         public AVSeekMap,
                         public std::enable_shared_from_this<AVMediaExtractor> {
public:
  /**
   * The output of the extractor, i.e. the tracks and the seek map of the media.
   */
  using Output = AVExtractorOutput;

  /**
   * The output of a single track, which receives the format and the samples of the track.
   */
  using TrackOutput = AVTrackOutput;

  /**
   * Holds the position the extractor of a platform asks its caller to read at.
   */
  using PositionHolder = AVPositionHolder;

  /**
   * The seek map of the media.
   */
  using SeekMap = AVSeekMap;

  /**
   * The outcome of {@link #read(AVExtractorInput&)}, i.e. the result of the extractor module.
   */
  using Result = AVExtractor::Result;

  /**
   * Result values that can be returned by {@link read(PositionHolder&)}, i.e. the three valued result
   * the extractors of the platforms are written with.
   */
  enum class PlatformResult : uint8_t {
    RESULT_END_OF_INPUT = 0,
    RESULT_CONTINUE = 1,
    RESULT_SEEK = 2,
  };

  enum class SeekOption : uint8_t {
    //!< If possible, seek to a sync sample at or before the specified time
    SEEK_TO_PREVIOUS_SYNC = 0,
    //!< If possible, seek to a sync sample at or after the specified time
    SEEK_TO_NEXT_SYNC = 1,
    //!< If possible, seek to the sync sample closest to the specified time
    SEEK_TO_CLOSEST_SYNC = 2,
  };

  virtual ~AVMediaExtractor() noexcept;

  AVMediaExtractor(const AVMediaExtractor& other) = delete;

  AVMediaExtractor& operator=(const AVMediaExtractor& other) = delete;

  /**
   * Initializes the extractor with the output that receives the tracks and the seek map. Called at
   * most once, before {@link #read}.
   *
   * <p>The output is owned by the reader, which keeps it alive for as long as it drives the extractor,
   * and it is never handed out again: an implementation stores it, and only uses it.
   *
   * @param output An {@link Output} to receive extracted data.
   */
  void init(AVExtractorOutput& output) override = 0;

  /**
   * Returns a value indicating whether this extractor recognizes the content of the input.
   *
   * <p>An extractor of a platform is created for a media it can read, e.g. the source reader of the
   * media foundation is only created when it opened the media, so by default it recognizes what it was
   * created for. An implementation that can tell more overrides it, and the value it returns decides
   * whether the reader drives it after a fallback selected it.
   *
   * @param input The input to peek from. An extractor of a platform reads its own media, so it may
   *              ignore it.
   *
   * @return One of the {@code kSniff*} constants declared in {@link AVExtractorsFactory}.
   */
  int sniff(AVExtractorInput& input) override;

  /**
   * Reads sample data, expressed the way the extractors of the platforms express it.
   *
   * <p>This is the read of a platform, which the interface of the extractor module bridges to: see
   * {@link #read(AVExtractorInput&)}. It must not be called before {@link #init}.
   *
   * @param seekPosition Updated to hold the position of the required data when
   *                     {@link PlatformResult::RESULT_SEEK} is returned.
   *
   * @return One of the {@link PlatformResult} values.
   */
  virtual PlatformResult read(PositionHolder& seekPosition) = 0;

  /**
   * Extracts data starting at the current position of the input.
   *
   * <p>This is the read of the extractor module: it bridges to {@link #read(PositionHolder&)}, and it
   * performs the seek that a {@link PlatformResult::RESULT_SEEK} asks for on the input of the reader.
   * An implementation of a platform must not override it.
   *
   * @param input The input to read from.
   *
   * @return One of the {@link Result} constants.
   */
  AVExtractor::Result read(AVExtractorInput& input) final override;

  /**
   * All selected tracks seek near the requested time according to the specified
   * mode.
   */
  bool seekTo(const int64_t timeUs) {
    return seekTo(timeUs, SeekOption::SEEK_TO_PREVIOUS_SYNC);
  }

  /**
   * All selected tracks seek near the requested time according to the specified
   * mode.
   */
  virtual bool seekTo(int64_t timeUs, SeekOption option) = 0;

  /**
   * Notifies the extractor that the input of the reader was repositioned.
   *
   * <p>The extractor of a platform reads its media itself, i.e. the input of the reader is not the
   * media it demuxes, so the byte position is only the position the reader is at: the media is seeked
   * by time.
   */
  bool seek(long position, int64_t timeUs) override;

  /**
   * Make sure you call this when you're done to free up any resources instead
   * of relying on the garbage collector to do this for you at some point in the
   * future.
   */
  virtual void clear() = 0;

  /**
   * Returns the duration of the Extractor, or {@link Definitions::kTimeUnset}
   * if the duration is unknown.
   */
  [[nodiscard]] int64_t getDurationUs() const override = 0;

  /**
   * Returns the current read position in the input stream, or {@link
   * Definitions::kPositionUnset} if no input is available.
   */
  [[nodiscard]] virtual int64_t getReadPosition() const = 0;

  /**
   * Returns whether seeking is supported.
   */
  [[nodiscard]] bool isSeekable() const override = 0;

  /**
   * Returns the seek points of the media, expressed the way the extractors of the platforms express
   * them.
   *
   * <p>This is the seek map of a platform, which the seek map of the extractor module bridges to: the
   * extractor of a platform implements this one and not {@link #getSeekPoints(int64_t, SeekPoint&,
   * SeekPoint&)}.
   *
   * @param timeUs A seek time in microseconds.
   *
   * @return The corresponding seek points, i.e. one point or the two points that surround the time.
   */
  [[nodiscard]] virtual coral::SeekPoints getPlatformSeekPoints(int64_t timeUs) const = 0;

  /**
   * Returns the seek points of the media, ordered by time, i.e. the list of the seek map of the
   * extractor module.
   *
   * <p>The extractors of the platforms resolve the seek point of a time on demand and do not enumerate
   * them, so the list is empty: a caller that needs a point asks for the time.
   */
  [[nodiscard]] const std::vector<SeekPoint>& getSeekPoints() const final;

  /**
   * Obtains the seek points that surround the given time, i.e. the seek map of the extractor module,
   * expressed with {@link #getPlatformSeekPoints(int64_t)}.
   */
  bool getSeekPoints(int64_t timeUs, SeekPoint& first, SeekPoint& second) const final;

protected:
  AVMediaExtractor() = default;

private:
  /**
   * The position the platform read asks its caller to read at, which every
   * {@link #read(PositionHolder&)} of the bridge is given. The holder has to be stable, because a
   * platform sets it when it returns {@link PlatformResult::RESULT_SEEK}.
   */
  AVPositionHolder platformPosition_;
};

/**
 * Factory for {@link AVMediaExtractor} instance.
 */
class ExtractorFactory {
public:
  virtual ~ExtractorFactory() = default;

  /**
   * Returns a new {@link AVMediaExtractor} instance.
   *
   * <p>The extractor is shared rather than owned by the caller alone: an extractor also hands itself out
   * as the {@link SeekMap} of the media it reads, and the output has to be able to keep it alive for as
   * long as it refers to it.
   */
  std::shared_ptr<AVMediaExtractor> createExtractor(const std::string& url) {
    return createExtractor(url, false);
  }

  /**
   * Returns a new {@link AVMediaExtractor} instance.
   */
  virtual std::shared_ptr<AVMediaExtractor> createExtractor(const std::string& url, bool platform) = 0;

  /**
   * Returns a new {@link AVMediaExtractor} that reads the media of an {@link AVExtractorInput}, i.e. the
   * media the reader of the extractor module owns, rather than the media of a url.
   *
   * <p>The extractor is not opened yet: it opens the media from the input, in {@link AVExtractor::sniff}.
   * The bytes such an extractor consumes are therefore the bytes the reader measures, which is what lets
   * the reader drive an extractor of a platform exactly as a built-in one, and which is why a container
   * that only the platform reads is playable by the reader. Only a platform whose extractor cannot read
   * the input of the reader returns null here, and its caller then falls back to
   * {@link #createExtractor}.
   *
   * @param url The url of the media, which is only what the diagnostics of the returned extractor report.
   */
  virtual std::shared_ptr<AVMediaExtractor> createInputExtractor(const std::string& url);
};

/**
 * Creates a new factory that can create the built-in types of extractors.
 */
std::unique_ptr<ExtractorFactory> CreateBuiltinExtractorFactory();

} // namespace coral

#endif // CORAL_CODEC_EXTRACTOR_H
