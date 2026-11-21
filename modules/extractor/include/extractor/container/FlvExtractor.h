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

#ifndef CORAL_EXTRACTOR_CONTAINER_FLV_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_FLV_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Extracts the audio and video tracks of an FLV container.
 *
 * <p>The container is a sequence of tags, each with an eleven byte header: a type, the size of the
 * payload, a timestamp and a stream identifier. The extractor understands the audio tag of an MP3,
 * AAC, A-law or mu-law stream, the video tag of an H.264 stream and the script tag that carries the
 * {@code onMetaData} object with the duration and the index of the key frames.
 *
 * <p>The payload of a video tag is length prefixed, while the platform decoders read Annex-B
 * framed samples: the extractor converts the framing of every sample with the length size that the
 * AVC configuration record of the stream declares.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code FlvExtractor},
 * {@code AudioTagPayloadReader}, {@code VideoTagPayloadReader} and
 * {@code ScriptTagPayloadReader}.
 */
class AVFlvExtractor final : public AVExtractor {
public:
  AVFlvExtractor();

  ~AVFlvExtractor() override = default;

  AVFlvExtractor(const AVFlvExtractor& other) = delete;

  AVFlvExtractor& operator=(const AVFlvExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "FLV"; }

  [[nodiscard]] std::string getSniffFailureReason() const override { return sniffFailureReason_; }

private:
  /**
   * The state of the tag parser.
   */
  enum class State {
    /** The nine byte FLV header has not been read yet. */
    kReadingFlvHeader,
    /** The parser skips to the header of the next tag. */
    kSkippingToTagHeader,
    /** The eleven byte tag header is being read. */
    kReadingTagHeader,
    /** The payload of the current tag is being read. */
    kReadingTagData,
  };

  /** Reads the FLV header and creates the tracks it declares. */
  bool readFlvHeader(AVExtractorInput& input);

  /** Reads the header of the next tag. */
  bool readTagHeader(AVExtractorInput& input);

  /** Reads and consumes the payload of the current tag. */
  bool readTagData(AVExtractorInput& input);

  /**
   * Consumes the payload of an audio tag.
   *
   * @return {@code true} when the tag produced a sample.
   */
  bool consumeAudioTag(int64_t timeUs);

  /**
   * Consumes the payload of a video tag.
   *
   * @return {@code true} when the tag produced a sample.
   */
  bool consumeVideoTag(int64_t timeUs);

  /**
   * Emits an audio sample, dropping the ones that precede the time of a seek that restarted the
   * container.
   *
   * @return {@code true} when the tag produced a sample.
   */
  bool emitAudioSample(const uint8_t* data, size_t size, int64_t timeUs, uint32_t flags);

  /**
   * Emits a video sample. A seek that restarted the container waits for the key frame that
   * precedes the requested time, because an inter frame cannot start the decoder.
   *
   * @return {@code true} when the tag produced a sample.
   */
  bool emitVideoSample(const uint8_t* data, size_t size, int64_t timeUs, uint32_t flags);

  /** Reports a framed sample to a track. */
  void deliverSample(const std::shared_ptr<AVTrackOutput>& track, const std::shared_ptr<AVBlockBuffer>& buffer,
    size_t size, int64_t timeUs, uint32_t flags);

  /** Consumes the payload of a script tag, i.e. the metadata of the stream. */
  void consumeScriptTag();

  /**
   * Parses the {@code onMetaData} object of a script tag.
   *
   * @return {@code true} when the duration or the key frame index was found.
   */
  bool parseMetadata();

  /** Publishes the audio format that the FLV audio header declares. */
  void publishAudioFormat(const std::string& mimeType, int sampleRate, int channelCount, int maxInputSize);

  /** Publishes the format of the AVC video track from its configuration record. */
  bool publishVideoFormat(const uint8_t* data, size_t size);

  /** Returns the timestamp that applies to the current tag. */
  [[nodiscard]] int64_t getCurrentTimestampUs() const;

  /** Publishes the seek map that the metadata declared, or an unseekable one. */
  void publishSeekMap(bool seekable, int64_t durationUs);

  /** Reports the end of both tracks. */
  void finish(AVExtractorInput& input);

  State state_ = State::kReadingFlvHeader;
  bool outputFirstSample_ = false;
  bool outputSeekMap_ = false;
  bool trackEnded_ = false;
  int bytesToNextTagHeader_ = 0;
  int tagType_ = 0;
  int tagDataSize_ = 0;
  int64_t tagTimestampUs_ = 0;
  int64_t mediaTagTimestampOffsetUs_ = 0;
  int64_t durationUs_ = Definitions::kTimeUnset;
  long position_ = 0;
  std::string sniffFailureReason_;

  /** The time of a seek that restarted the container, and the state of its skip. */
  int64_t seekTargetUs_ = -1;
  bool waitingForVideoSeek_ = false;
  bool waitingForAudioSeek_ = false;
  std::shared_ptr<AVBlockBuffer> pendingKeyframeData_;
  size_t pendingKeyframeSize_ = 0;
  int64_t pendingKeyframeTimeUs_ = Definitions::kTimeUnset;
  uint32_t pendingKeyframeFlags_ = 0;

  // The audio state.
  bool audioHeaderParsed_ = false;
  bool audioFormatPublished_ = false;
  bool audioUnsupported_ = false;
  int audioFormat_ = -1;
  std::shared_ptr<AVTrackOutput> audioOutput_;

  // The video state.
  bool videoFormatPublished_ = false;
  bool videoUnsupported_ = false;
  bool hasOutputKeyframe_ = false;
  int nalUnitLengthFieldLength_ = 0;
  int frameType_ = 0;
  std::shared_ptr<AVTrackOutput> videoOutput_;

  // The metadata state.
  std::vector<int64_t> keyFrameTimesUs_;
  std::vector<long> keyFramePositions_;

  AVExtractorOutput* output_ = nullptr;
  AVDataBuffer headerBuffer_;
  AVDataBuffer tagHeaderBuffer_;
  AVDataBuffer tagData_;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_CONTAINER_FLV_EXTRACTOR_H
