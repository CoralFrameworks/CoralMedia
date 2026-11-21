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

#ifndef CORAL_EXTRACTOR_EXTRACTOR_H
#define CORAL_EXTRACTOR_EXTRACTOR_H

#include <extractor/ExtractorInput.h>

#include <media/BlockBuffer.h>
#include <media/Buffer.h>
#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coral {

class AVSeekMap;

/**
 * A container for media samples and the corresponding metadata.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code Sample}, and it is the unit of
 * data that an {@link AVExtractor} pushes to its {@link AVExtractorOutput}.
 *
 * <p>The class owns its payload through a shared {@link AVBlockBuffer}, therefore an instance may
 * safely outlive the extractor that produced it. Instances are only ever moved, never copied.
 */
class AVSample final {
public:
  /**
   * The extractor module counterpart of ExoPlayer's {@code C.BUFFER_FLAG_*} constants.
   */
  enum Flags : uint32_t {
    //!< Default flags.
    kFlagNone = 0,
    //!< The sample is a sync sample (a key frame).
    kFlagKeyFrame = 1,
    //!< The sample contains codec initialization data, i.e. it is not media data.
    kFlagCodecConfig = 1 << 1,
    //!< The sample signals the end of the stream.
    kFlagEndOfStream = 1 << 2,
    //!< The sample is the first media sample of the stream.
    kFlagFirstSample = 1 << 27,
    //!< The sample carries supplemental data.
    kFlagHasSupplementalData = 1 << 28,
    //!< The sample is the last media sample of the stream.
    kFlagLastSample = 1 << 29,
    //!< The sample should be decoded but not rendered.
    kFlagDiscard = 1 << 30,
  };

  /** The index of the track this sample belongs to, or {@link Definitions::kIndexUnset}. */
  int trackIndex;

  /** The presentation timestamp of the sample, in microseconds. */
  int64_t timeUs;

  /** A combination of the {@link Flags} constants. */
  uint32_t flags;

  /** The payload of the sample. Never null for a non end of stream sample. */
  std::shared_ptr<AVBlockBuffer> data;

  AVSample() noexcept
    : trackIndex(Definitions::kIndexUnset),
      timeUs(Definitions::kTimeUnset),
      flags(static_cast<uint32_t>(kFlagNone)) {
  }

  AVSample(const int trackIndex, const int64_t timeUs, const uint32_t flags,
           std::shared_ptr<AVBlockBuffer> data) noexcept
    : trackIndex(trackIndex),
      timeUs(timeUs),
      flags(flags),
      data(std::move(data)) {
  }

  AVSample(const AVSample& other) = delete;

  AVSample& operator=(const AVSample& other) = delete;

  AVSample(AVSample&& other) noexcept
    : trackIndex(other.trackIndex),
      timeUs(other.timeUs),
      flags(other.flags),
      data(std::move(other.data)) {
  }

  AVSample& operator=(AVSample&& other) noexcept {
    if (this != &other) {
      trackIndex = other.trackIndex;
      timeUs = other.timeUs;
      flags = other.flags;
      data = std::move(other.data);
    }
    return *this;
  }

  ~AVSample() = default;

  /**
   * Returns whether the given flag is set.
   */
  [[nodiscard]] bool hasFlag(const uint32_t flag) const {
    return (flags & flag) == flag;
  }

  /** Returns whether this sample is a sync sample. */
  [[nodiscard]] bool isKeyFrame() const { return hasFlag(kFlagKeyFrame); }

  /** Returns whether this sample carries codec initialization data. */
  [[nodiscard]] bool isCodecConfig() const { return hasFlag(kFlagCodecConfig); }

  /** Returns whether this sample signals the end of the stream. */
  [[nodiscard]] bool isEndOfStream() const { return hasFlag(kFlagEndOfStream); }

  /** Returns the size of the payload in bytes. */
  [[nodiscard]] size_t size() const {
    return data != nullptr ? data->limit() : 0;
  }

  /** Returns a read only pointer to the payload, or null when there is no payload. */
  [[nodiscard]] const uint8_t* array() const {
    return data != nullptr ? data->array() : nullptr;
  }

  /** Updates the presentation timestamp of the sample. */
  void setTimeUs(const int64_t newTimeUs) { timeUs = newTimeUs; }

  /** Adds the given flag to the sample. */
  void addFlag(const uint32_t flag) { flags |= flag; }

  /** Returns a human readable description of the sample. */
  [[nodiscard]] std::string toString() const;
};

/**
 * Returns the flags of an {@link AVSample} that correspond to the given flags of a media buffer.
 *
 * <p>Both sets are the {@code C.BUFFER_FLAG_*} constants of ExoPlayer, and they carry the same
 * values, which the assertions below pin down at compile time: the buffer of a media sample and the
 * sample of an extractor describe the same thing, and a module that converts one into the other must
 * not restate the values. This is the single place where the flags of the codec module are translated
 * into the flags of the extractor module.
 */
[[nodiscard]] constexpr uint32_t toSampleFlags(const AVBuffer::Flags flags) noexcept {
  return static_cast<uint32_t>(flags);
}

/**
 * Returns the flags of a media buffer that correspond to the given flags of an {@link AVSample}, i.e.
 * the inverse of {@link toSampleFlags}.
 *
 * <p>The two conversions are the only places where the flags of the codec module meet the flags of the
 * extractor module, and the assertions above hold both of them to the same values.
 */
[[nodiscard]] constexpr AVBuffer::Flags toBufferFlags(const uint32_t flags) noexcept {
  return static_cast<AVBuffer::Flags>(flags);
}

static_assert(static_cast<uint32_t>(AVBuffer::Flags::kNone) == AVSample::kFlagNone,
    "the default flags must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kKeyFrame) == AVSample::kFlagKeyFrame,
    "the sync sample flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kCodecConfig) == AVSample::kFlagCodecConfig,
    "the codec configuration flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kEndOfStream) == AVSample::kFlagEndOfStream,
    "the end of stream flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kFirstSample) == AVSample::kFlagFirstSample,
    "the first sample flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kHasSupplementalData) ==
    AVSample::kFlagHasSupplementalData, "the supplemental data flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kLastSample) == AVSample::kFlagLastSample,
    "the last sample flag must agree");
static_assert(static_cast<uint32_t>(AVBuffer::Flags::kDiscard) == AVSample::kFlagDiscard,
    "the discard flag must agree");

/**
 * The output of a single track, as returned by {@link AVExtractorOutput::track}.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code TrackOutput}. Unlike the Java
 * implementation, the sample is delivered to the callback that the reader installed for the track,
 * which is resolved when the track is created.
 *
 * <p>The class is a thin, thread safe handle: the callback and the format are held by the
 * {@link AVTrackInfo} of the track, so that a consumer that is installed or replaced after the
 * track was created is still taken into account.
 */
class AVTrackOutput {
public:
  /** The callback invoked for every extracted sample. */
  using SampleConsumer = std::function<void(const AVSample&)>;

  /** A callback that does nothing, usable as a placeholder. */
  static const SampleConsumer& emptyConsumer();

  /**
   * The receiver of the samples of a track. The reader owns the implementation, the output only
   * forwards to it.
   */
  class Consumer {
  public:
    virtual ~Consumer() = default;

    /** Delivers a sample to the track. */
    virtual void consume(const AVSample& sample) = 0;

    /** Reports the format of the samples that will follow. */
    virtual void setFormat(const std::shared_ptr<MediaFormat>& format) = 0;
  };

  AVTrackOutput(const int id, std::shared_ptr<Consumer> consumer) noexcept
    : id_(id),
      consumer_(std::move(consumer)) {
  }

  AVTrackOutput(const AVTrackOutput& other) = delete;

  AVTrackOutput& operator=(const AVTrackOutput& other) = delete;

  ~AVTrackOutput() = default;

  /** Returns the identifier of the track, as passed to {@link AVExtractorOutput::track}. */
  [[nodiscard]] int id() const { return id_; }

  /**
   * Called when the format of the track is known, or when it changes.
   *
   * @param format The format of the samples that will follow.
   */
  void format(const std::shared_ptr<MediaFormat>& format) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      format_ = format;
    }
    if (consumer_ != nullptr) {
      consumer_->setFormat(format);
    }
  }

  /**
   * Returns the last format that was reported for this track, possibly null.
   */
  [[nodiscard]] std::shared_ptr<MediaFormat> getFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
  }

  /**
   * Consumes a sample produced by an extractor.
   *
   * <p>The sample is delivered by const reference; an implementation that needs to keep the payload
   * copies it. The buffer itself is reference counted, so it stays valid as long as it is
   * referenced.
   */
  void sample(const AVSample& sample) {
    if (consumer_ != nullptr) {
      consumer_->consume(sample);
    }
  }

  /** Signals the end of the samples of this track. */
  void endTrack();

private:
  int id_;
  std::shared_ptr<Consumer> consumer_;
  std::shared_ptr<MediaFormat> format_;
  mutable std::mutex mutex_;
};

/**
 * Receives the tracks and the seek map of a container, as produced by an {@link AVExtractor}.
 */
class AVExtractorOutput {
public:
  virtual ~AVExtractorOutput() = default;

  /**
   * Called when a track is discovered.
   *
   * @param id The identifier of the track, unique within the container.
   * @param type The media type of the track.
   * @param consumer The callback that will receive the samples of the track. It may be empty, in
   *                 which case the samples of the track are dropped.
   *
   * @return The track output that the extractor must use to deliver the samples of the track.
   */
  virtual std::shared_ptr<AVTrackOutput>
  track(int id, MediaType type, AVTrackOutput::SampleConsumer consumer) = 0;

  /**
   * Called when a track is discovered, without any assumption about its media type.
   */
  virtual std::shared_ptr<AVTrackOutput> track(int id, AVTrackOutput::SampleConsumer consumer) = 0;

  /**
   * Called when the seek map of the container is known.
   *
   * <p>May be called again when the container reveals more accurate information, e.g. a duration
   * that only becomes known once the header was parsed: the last call wins.
   */
  virtual void seekMap(std::shared_ptr<AVSeekMap> seekMap) = 0;

  /**
   * Called when all the tracks of the container have been discovered, meaning that no more
   * {@link #track} call will be made.
   *
   * <p>This is the extractor module counterpart of ExoPlayer's {@code ExtractorOutput.endTracks},
   * and it is what tells a caller that it can stop waiting for tracks: a container whose tracks are
   * all unsupported reports no track at all, and only this notification tells that case apart from
   * a container whose tracks were not parsed yet.
   *
   * <p>The default implementation does nothing.
   */
  virtual void endTracks() {}
};

/**
 * Defines how to seek in a container.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code SeekMap}.
 */
class AVSeekMap {
public:
  virtual ~AVSeekMap() = default;

  /**
   * A pair of a time and the position in the stream where that time can be found.
   */
  struct SeekPoint {
    /** The time of the seek point, in microseconds. */
    int64_t timeUs;

    /** The byte position of the seek point in the stream. */
    long position;

    SeekPoint() noexcept : timeUs(Definitions::kTimeUnset), position(-1) {}

    SeekPoint(const int64_t timeUs, const long position) noexcept
      : timeUs(timeUs),
        position(position) {
    }

    [[nodiscard]] std::string toString() const;
  };

  /**
   * Returns whether the container supports seeking.
   */
  [[nodiscard]] virtual bool isSeekable() const = 0;

  /**
   * Returns the duration of the container in microseconds, or {@link Definitions::kTimeUnset} if
   * unknown.
   */
  [[nodiscard]] virtual int64_t getDurationUs() const = 0;

  /**
   * Returns the seek points of the container, ordered by time. Returns an empty vector when
   * {@link isSeekable()} is false.
   */
  [[nodiscard]] virtual const std::vector<SeekPoint>& getSeekPoints() const = 0;

  /**
   * Obtains the seek points that surround the given time.
   *
   * <p>This is the extractor module counterpart of ExoPlayer's {@code SeekMap.getSeekPoints}: a
   * caller that is allowed to seek within a tolerance needs both neighbours to pick the closer one,
   * which a single point cannot express.
   *
   * @param timeUs The seek time, in microseconds.
   * @param first Filled in with the last seek point at or before {@code timeUs}, or with the first
   *              seek point of the map when {@code timeUs} precedes all of them.
   * @param second Filled in with the first seek point after {@code timeUs}, or with {@code first}
   *               when there is none.
   *
   * @return Whether the seek map could resolve the given time.
   */
  virtual bool getSeekPoints(int64_t timeUs, SeekPoint& first, SeekPoint& second) const = 0;

  /**
   * Obtains a {@link SeekPoint} for the given time.
   *
   * @param timeUs The seek time, in microseconds.
   * @param out The seek point that will be filled in. Left untouched when the seek map is not
   *            seekable.
   *
   * @return Whether the seek map could resolve the given time.
   */
  bool getSeekPoint(const int64_t timeUs, SeekPoint& out) const {
    SeekPoint ignored;
    return getSeekPoints(timeUs, out, ignored);
  }
};

/**
 * A seek map that does not support seeking.
 */
class AVUnseekableSeekMap final : public AVSeekMap {
public:
  explicit AVUnseekableSeekMap(const int64_t durationUs) noexcept
    : durationUs_(durationUs) {
  }

  [[nodiscard]] bool isSeekable() const override { return false; }

  [[nodiscard]] int64_t getDurationUs() const override { return durationUs_; }

  [[nodiscard]] const std::vector<SeekPoint>& getSeekPoints() const override {
    static const std::vector<SeekPoint> sEmpty;
    return sEmpty;
  }

  bool getSeekPoints(const int64_t, SeekPoint&, SeekPoint&) const override { return false; }

private:
  int64_t durationUs_;
};

/**
 * A seek map built from an explicit list of seek points.
 */
class AVIndexSeekMap final : public AVSeekMap {
public:
  /**
   * @param seekPoints The seek points, ordered by time and position.
   * @param durationUs The duration of the container in microseconds.
   * @param isSeekable Whether the container supports seeking. When false, the seek points are
   *                   ignored but still reported.
   */
  AVIndexSeekMap(std::vector<SeekPoint> seekPoints, const int64_t durationUs, const bool isSeekable) noexcept
    : seekPoints_(std::move(seekPoints)),
      durationUs_(durationUs),
      isSeekable_(isSeekable) {
  }

  [[nodiscard]] bool isSeekable() const override { return isSeekable_; }

  [[nodiscard]] int64_t getDurationUs() const override { return durationUs_; }

  [[nodiscard]] const std::vector<SeekPoint>& getSeekPoints() const override {
    return seekPoints_;
  }

  bool getSeekPoints(int64_t timeUs, SeekPoint& first, SeekPoint& second) const override;

private:
  std::vector<SeekPoint> seekPoints_;
  int64_t durationUs_;
  bool isSeekable_;
};

/**
 * Extracts the tracks, the samples and the seek map of a container.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code Extractor}. An extractor is
 * owned by an {@link AVReader}, which drives it until the end of the input is reached. An extractor
 * is expected to be used from a single thread at a time, as enforced by the reader.
 */
class AVExtractor {
public:
  /**
   * The outcome of {@link #read}.
   */
  enum Result : int {
    /** The extractor consumed input and made progress. */
    kResultContinue = 0,

    /**
     * The extractor consumed all the input it was given for the current position, and the reader
     * must advance the input before calling {@link #read} again.
     */
    kResultEndOfInput = 1,
  };

  /** The maximum number of bytes the {@link #sniff} implementations may peek. */
  static constexpr int kSniffPeekLimit = 64 * 1024;

  virtual ~AVExtractor() = default;

  AVExtractor(const AVExtractor& other) = delete;

  AVExtractor& operator=(const AVExtractor& other) = delete;

  /**
   * Returns a value indicating whether this extractor recognizes the content of the input.
   *
   * @param input The input to peek from. Its peek position is restored on return.
   *
   * @return One of the {@code kSniff*} constants declared in {@link AVExtractorsFactory}.
   */
  virtual int sniff(AVExtractorInput& input) = 0;

  /**
   * Initializes the extractor. Called exactly once, before {@link #read}.
   *
   * @param output The output that receives the tracks and the seek map.
   */
  virtual void init(AVExtractorOutput& output) = 0;

  /**
   * Extracts data starting at the current position of the input.
   *
   * @param input The input to read from.
   *
   * @return One of the {@link Result} constants.
   */
  virtual Result read(AVExtractorInput& input) = 0;

  /**
   * Called when a seek is requested on the input.
   *
   * <p>The default implementation resets the extractor state with respect to the new position and
   * reports success so that the reader simply re-reads the container from the new position.
   */
  virtual bool seek(const long position, const int64_t timeUs);

  /**
   * Returns whether this extractor needs to be re-created when the input position changes.
   */
  virtual bool requiresReloadAfterSeek() const { return false; }

  /**
   * Returns the number of bytes read from the input while the extractor processed the current
   * position. Used by the reader to update its position.
   */
  [[nodiscard]] virtual long getPosition() const { return 0; }

  /**
   * Returns the name of the container handled by this extractor, for diagnostics.
   */
  [[nodiscard]] virtual std::string getName() const = 0;

  /**
   * Returns a description of why {@link #sniff} rejected the input, for diagnostics.
   */
  [[nodiscard]] virtual std::string getSniffFailureReason() const { return {}; }

protected:
  AVExtractor() = default;
};

/**
 * Creates {@link AVExtractor} instances, and recognizes the container type of the input.
 */
class AVExtractorsFactory {
public:
  /**
   * The returned confidence of {@code sniff}: the content cannot be handled at all.
   */
  static constexpr int kSniffResultNone = 0;

  /**
   * The returned confidence of {@code sniff}: the content may be handled.
   */
  static constexpr int kSniffResultPossible = 1;

  /**
   * The returned confidence of {@code sniff}: the content is definitely handled.
   */
  static constexpr int kSniffResultCertain = 2;

  virtual ~AVExtractorsFactory() = default;

  /**
   * Called once per read loop to create the extractors that will be asked to sniff the input.
   *
   * @return The extractors, in decreasing order of confidence. Must not be null.
   */
  virtual std::vector<std::unique_ptr<AVExtractor>> createExtractors() = 0;

  /**
   * Called once per read loop to create the extractors that will be asked to sniff the input.
   *
   * @param uri The uri of the media, if known.
   * @param responseHeaders The response headers of the media, if known.
   */
  virtual std::vector<std::unique_ptr<AVExtractor>>
  createExtractors(const std::string& uri, const std::vector<std::string>& responseHeaders);

  /**
   * Returns a factory creating the extractors for the container formats supported natively by this
   * module: MP4, Matroska/WebM, Ogg, AVI, FLV, MPEG-TS, MP3, ADTS, AMR, WAV, FLAC, BMP, PNG, JPEG,
   * WebP, AVIF and HEIF.
   *
   * @param fileType The type of the container, as returned by
   *                 {@link MediaFileType} related helpers, or {@code -1} when unknown.
   * @param length The length of the media in bytes, or {@code -1} when unknown.
   *
   * @return A factory. Never null.
   */
  static std::unique_ptr<AVExtractorsFactory> createDefault(int fileType = -1, long length = -1);
};

} // namespace coral

#endif //CORAL_EXTRACTOR_EXTRACTOR_H
