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

#ifndef CORAL_EXTRACTOR_READER_H
#define CORAL_EXTRACTOR_READER_H

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorInput.h>
#include <extractor/Extractor.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coral {

/**
 * Describes a track that was discovered by an extractor, together with the callback that receives
 * its samples.
 */
class AVTrackInfo final : public AVTrackOutput::Consumer {
public:
  AVTrackInfo(const int id, const MediaType type, AVTrackOutput::SampleConsumer consumer) noexcept
    : id_(id),
      type_(type),
      consumer_(std::move(consumer)) {
  }

  AVTrackInfo(const AVTrackInfo& other) = delete;

  AVTrackInfo& operator=(const AVTrackInfo& other) = delete;

  ~AVTrackInfo() override = default;

  [[nodiscard]] int id() const { return id_; }

  [[nodiscard]] MediaType getType() const { return type_; }

  void setType(const MediaType type) { type_ = type; }

  /**
   * Installs the callback that receives the samples of the track.
   *
   * <p>The reader installs the callback of a track as soon as the track is created, so a consumer
   * registered before {@link AVReader::prepare} is called already receives the first samples. A
   * consumer may also be replaced at any time, the replacement is serialized with the deliveries.
   */
  void setConsumer(AVTrackOutput::SampleConsumer consumer) {
    std::lock_guard<std::mutex> lock(mutex_);
    consumer_ = std::move(consumer);
  }

  /**
   * Returns the last format that was reported for this track, possibly null.
   */
  [[nodiscard]] std::shared_ptr<MediaFormat> getFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
  }

  /**
   * Delivers a sample to the consumer of the track. The delivery is serialized so that a consumer
   * implementation that is not itself thread safe is still safe to use.
   */
  void consume(const AVSample& sample) override {
    AVTrackOutput::SampleConsumer consumer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      consumer = consumer_;
    }
    if (consumer) {
      consumer(sample);
    }
  }

  /** Reports the format of the samples that will follow. */
  void setFormat(const std::shared_ptr<MediaFormat>& format) override {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
  }

private:
  int id_;
  MediaType type_;
  AVTrackOutput::SampleConsumer consumer_;
  std::shared_ptr<MediaFormat> format_;
  mutable std::mutex mutex_;
};

/**
 * The {@link AVExtractorOutput} used by {@link AVReader}.
 *
 * <p>The reader owns one instance, which collects the tracks and the seek map declared by the
 * extractor that was selected.
 */
class AVReaderOutput final : public AVExtractorOutput {
public:
  /**
   * The callback that resolves the consumer to install on a newly created track. It is called with
   * the identifier of the track.
   */
  using ConsumerResolver = std::function<AVTrackOutput::SampleConsumer(int)>;

  explicit AVReaderOutput(ConsumerResolver resolver) noexcept
    : resolver_(std::move(resolver)) {
  }

  ~AVReaderOutput() override = default;

  AVReaderOutput(const AVReaderOutput& other) = delete;

  AVReaderOutput& operator=(const AVReaderOutput& other) = delete;

  std::shared_ptr<AVTrackOutput> track(int id, MediaType type,
                                       AVTrackOutput::SampleConsumer consumer) override;

  std::shared_ptr<AVTrackOutput> track(int id, AVTrackOutput::SampleConsumer consumer) override;

  void seekMap(std::shared_ptr<AVSeekMap> seekMap) override;

  /**
   * Called by the track output of a track when its format is known, so that the reader can expose
   * the format before the first sample of the track is delivered.
   */
  void setFormat(int id, std::shared_ptr<MediaFormat> format);

  /**
   * Returns the track with the given identifier, or null when it does not exist.
   */
  [[nodiscard]] std::shared_ptr<AVTrackInfo> getTrack(int id) const;

  /**
   * Returns the identifiers of the tracks that were discovered so far, ordered by identifier.
   */
  [[nodiscard]] std::vector<int> getTrackIds() const;

  /**
   * Returns the seek map declared by the extractor, or null when the extractor did not declare one.
   */
  [[nodiscard]] std::shared_ptr<AVSeekMap> getSeekMap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seekMap_;
  }

  /**
   * Returns the duration of the media in microseconds, or {@link Definitions::kTimeUnset}.
   */
  [[nodiscard]] int64_t getDurationUs() const;

private:
  ConsumerResolver resolver_;
  mutable std::mutex mutex_;
  std::map<int, std::shared_ptr<AVTrackInfo>> tracks_;
  std::shared_ptr<AVSeekMap> seekMap_;
};

/**
 * Reads the samples of a container by driving an {@link AVExtractor}.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code DefaultExtractorInput} based
 * reading loop, and it is the main entry point of the module. A reader owns its data source, the
 * input and the selected extractor, so that destroying the reader releases every resource that was
 * acquired, in reverse order of acquisition.
 *
 * <p>The reader is safe to use from multiple threads: the internal state is protected by a mutex,
 * and the sample consumers are invoked under their own lock.
 *
 * <h3>Usage</h3>
 * <pre>
 *   auto dataSource = std::make_shared<AVFileDataSource>();
 *   AVReader reader(dataSource, AVExtractorsFactory::createDefault(fileType, length));
 *   reader.setTrackConsumer(0, [](const AVSample& sample) { ... });
 *   reader.prepare();
 *   while (reader.read() > 0) {
 *   }
 * </pre>
 */
class AVReader final {
public:
  /**
   * The outcome of {@link #read}.
   */
  enum Result : int {
    /** The extractor consumed input but did not reach the requested position yet. */
    kResultContinue = 0,

    /** The requested position was reached, or the end of the input was reached. */
    kResultEndOfInput = 1,

    /** An error occurred, e.g. no extractor recognized the container. */
    kResultError = -1,
  };

  /**
   * @param dataSource The data source to read from. Must not be null.
   * @param extractorsFactory The factory creating the extractors that will be asked to sniff the
   *                          input. Must not be null.
   * @param fileType The type of the container, as defined by {@link FileTypes}, or -1 when unknown.
   */
  AVReader(std::shared_ptr<AVDataSource> dataSource,
           std::unique_ptr<AVExtractorsFactory> extractorsFactory, int fileType = -1);

  ~AVReader();

  AVReader(const AVReader& other) = delete;

  AVReader& operator=(const AVReader& other) = delete;

  /**
   * Opens the data source, probes the container and creates the tracks and the seek map.
   *
   * @return {@code true} on success.
   */
  bool prepare();

  /**
   * Reads the next chunk of media.
   *
   * @return {@link #kResultContinue} when there is more media to read at the current position,
   *         {@link #kResultEndOfInput} when the requested position or the end of the input was
   *         reached, or {@link #kResultError} on failure.
   */
  int read();

  /**
   * Requests the next read to start at the given position.
   *
   * @param position The byte position to seek to, or -1 to keep the current position.
   * @param timeUs The time to seek to, in microseconds, used to resolve the position when the
   *               container is seekable, or -1 when unknown.
   *
   * @return {@code true} when the seek can be performed.
   */
  bool seek(long position, int64_t timeUs);

  /**
   * Signals the end of the samples of every track that was discovered.
   *
   * <p>An extractor only signals the end of a track when it observes it, e.g. when it reaches the
   * end of the data chunk it was reading. A caller that stopped reading because the requested
   * position or the end of the input was reached calls this method to close the tracks explicitly.
   */
  void endTracks();

  /**
   * Returns the current byte position of the reader.
   */
  [[nodiscard]] long getPosition() const;

  /**
   * Returns the duration of the media in microseconds, or {@link Definitions::kTimeUnset}.
   */
  [[nodiscard]] int64_t getDurationUs() const;

  /**
   * Returns the seek map of the container, or null when it is not known yet.
   */
  [[nodiscard]] std::shared_ptr<AVSeekMap> getSeekMap() const;

  /**
   * Returns the track with the given identifier, or null when it does not exist.
   */
  [[nodiscard]] std::shared_ptr<AVTrackInfo> getTrack(int id) const;

  /**
   * Returns the identifiers of the tracks that were discovered so far.
   */
  [[nodiscard]] std::vector<int> getTrackIds() const;

  /**
   * Returns the format of the given track, or null when it is not known yet.
   */
  [[nodiscard]] std::shared_ptr<MediaFormat> getTrackFormat(int id) const;

  /**
   * Installs the callback that receives the samples of a track.
   *
   * <p>The callback is invoked while the reader holds its internal lock, therefore an
   * implementation must not call back into the reader. It must be quick, or hand the sample over to
   * another thread.
   *
   * @param id The identifier of the track.
   * @param consumer The callback. An empty callback drops the samples of the track.
   */
  void setTrackConsumer(int id, AVTrackOutput::SampleConsumer consumer);

  /**
   * Returns the underlying data source.
   */
  [[nodiscard]] const std::shared_ptr<AVDataSource>& getDataSource() const { return dataSource_; }

  /**
   * Returns the extractor that was selected by the probe, or null when none was.
   */
  [[nodiscard]] const AVExtractor* getSelectedExtractor() const { return extractor_.get(); }

  /**
   * Returns the name of the selected extractor, or an empty string when none was selected.
   */
  [[nodiscard]] std::string getSelectedExtractorName() const {
    return extractor_ != nullptr ? extractor_->getName() : std::string();
  }

  /**
   * Returns a description of the last failure, or an empty string.
   */
  [[nodiscard]] std::string getLastError() const;

  /**
   * Returns the type of the container, as defined by {@link FileTypes}, or -1 when unknown.
   */
  [[nodiscard]] int getFileType() const;

private:
  bool prepareInternalLocked();

  int readInternalLocked();

  /**
   * Selects the extractor that recognizes the container.
   */
  bool sniffExtractorsLocked();

  /**
   * Resolves the position of the pending seek with the help of the seek map.
   */
  bool resolveSeekLocked();

  /**
   * The maximum number of times the extractor is called by a single {@link #read}.
   */
  static constexpr int kMaxIterationsPerRead = 16;

  std::shared_ptr<AVDataSource> dataSource_;
  std::unique_ptr<AVExtractorsFactory> extractorsFactory_;
  std::shared_ptr<AVReaderOutput> output_;
  std::unique_ptr<AVDefaultExtractorInput> input_;
  std::unique_ptr<AVExtractor> extractor_;

  mutable std::recursive_mutex mutex_;
  bool prepared_ = false;
  bool endOfInputReached_ = false;
  bool tracksEnded_ = false;
  long position_ = 0;
  long pendingPosition_ = -1;
  int64_t pendingTimeUs_ = -1;
  bool seekPending_ = false;
  /**
   * The position that the reader must reach before {@link #read} reports the end of the input, or
   * -1 when no seek is being resolved.
   */
  long seekTarget_ = -1;
  int fileType_;
  int64_t durationUs_ = Definitions::kTimeUnset;
  std::string lastError_;

  /**
   * The consumers that were registered before the tracks were created. It has its own mutex because
   * the tracks resolve their consumer from a thread that already holds the reader lock.
   */
  std::mutex consumersMutex_;
  std::map<int, AVTrackOutput::SampleConsumer> consumers_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_READER_H
