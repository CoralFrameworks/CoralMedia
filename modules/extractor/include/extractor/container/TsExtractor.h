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

#ifndef CORAL_EXTRACTOR_CONTAINER_TS_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_TS_EXTRACTOR_H

#include <extractor/Extractor.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace coral {

class AVTsSeekMap;

/**
 * Extracts the audio and video tracks of an MPEG-2 transport stream.
 *
 * <p>The extractor follows ISO/IEC 13818-1: it reads the program association table, then the
 * program map table, and creates one track for every elementary stream that it supports. The
 * payload of an elementary stream is carried by PES packets, which are reassembled across the 188
 * byte transport packets before an elementary stream reader consumes them.
 *
 * <p>The supported elementary streams are H.264, H.265, AAC in ADTS framing and MPEG audio. The
 * codec configuration of an H.264 or H.265 stream is taken from the parameter set NAL units that
 * the stream carries, and the AudioSpecificConfig of an AAC stream is synthesized from the first
 * ADTS header.
 *
 * <p>The timestamps of a PES packet are stored in 90 kHz units. They are adjusted so that the first
 * sample of a stream starts at zero, or at the position that a seek requested, which is what
 * ExoPlayer's TimestampAdjuster does. The index of the key frames that the extractor reads
 * becomes the seek map.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's TsExtractor, restricted to the
 * single program mode and to the elementary streams that the Coral decoders render.
 */
class AVTsExtractor final : public AVExtractor {
public:
  /** The size of a transport packet, in bytes. */
  static constexpr int kPacketSize = 188;

  /** The number of transport packets that the sniffing pattern checks. */
  static constexpr int kSniffPacketCount = 5;

  AVTsExtractor();

  ~AVTsExtractor() override = default;

  AVTsExtractor(const AVTsExtractor& other) = delete;

  AVTsExtractor& operator=(const AVTsExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "TS"; }

  [[nodiscard]] std::string getSniffFailureReason() const override { return sniffFailureReason_; }

private:
  /**
   * One elementary stream of the program, and the state that reassembles and parses it.
   */
  struct Stream {
    int pid = -1;
    int streamType = -1;
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    std::shared_ptr<AVTrackOutput> output;
    /** The caption track derived from the SEI of a video stream, created on first use. */
    std::shared_ptr<AVTrackOutput> captionOutput;
    /** The CEA-608 caption track derived from the same SEI. */
    std::shared_ptr<AVTrackOutput> caption608Output;

    /** Whether the format of the track was published. */
    bool formatPublished = false;

    /** The PES packet that is being reassembled. */
    std::vector<uint8_t> pesData;
    bool pesActive = false;
    long pesStartPosition = -1;
    int pesExpectedSize = -1;
    int64_t pesTimeUs = Definitions::kTimeUnset;

    /** The last timestamp that was emitted, used when a PES packet has no PTS. */
    int64_t lastSampleTimeUs = Definitions::kTimeUnset;

    /** Whether the track already received a sample, so that the first sample flag is set once. */
    bool hasEmittedSample = false;

    /**
     * The time that a seek is waiting for. A seek that had no index entry restarts the input at
     * zero, therefore the samples before the requested time are dropped; the video stream starts
     * at the key frame that precedes it, because a decoder cannot start at an inter frame.
     */
    int64_t seekTargetUs = -1;
    bool waitingForSeek = false;
    std::shared_ptr<AVBlockBuffer> pendingKeyframeData;
    size_t pendingKeyframeSize = 0;
    int64_t pendingKeyframeTimeUs = Definitions::kTimeUnset;
    uint32_t pendingKeyframeFlags = 0;

    /** The parameter sets of an H.264 or H.265 stream. */
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;

    /** The audio parameters of an AAC or MPEG audio stream. */
    int sampleRate = 0;
    int channelCount = 0;
    int audioObjectType = 2;
    int audioSpecificConfig = 0;
  };

  /**
   * Reads a chunk of input, then parses every complete transport packet that the buffer holds.
   *
   * @return false when the end of the input was reached.
   */
  bool fillAndParse(AVExtractorInput& input);

  /**
   * Appends the bytes that were read to the transport packet buffer.
   */
  void appendToBuffer(const uint8_t* data, size_t size);

  /**
   * Drops the bytes that were already parsed and shifts the rest to the start of the buffer.
   */
  void compactBuffer();

  /**
   * Finds the first transport packet in the buffer, drops the bytes that precede it and returns
   * whether a complete packet is available.
   */
  bool findPacket();

  /**
   * Parses one complete transport packet.
   *
   * @param packet The 188 bytes of the packet.
   * @param position The absolute position of the packet in the input.
   */
  void processPacket(const uint8_t* packet, long position);

  /** Parses a program association table section. */
  void parsePat(const uint8_t* section, size_t size);

  /** Parses a program map table section. */
  void parsePmt(const uint8_t* section, size_t size);

  /** Creates the track of an elementary stream that the program map table declared. */
  void createStream(int pid, int streamType);

  /** Appends the payload of a transport packet to the PES packet of a stream. */
  void appendPesPayload(Stream& stream, const uint8_t* payload, size_t size, bool payloadUnitStart, long position);

  /** Parses the PES packet of a stream that is complete and consumes its body. */
  void processPes(Stream& stream);

  /** Consumes the body of an H.264 PES packet. */
  void consumeH264(Stream& stream, const uint8_t* data, size_t size, int64_t timeUs);

  /** Consumes the body of an H.265 PES packet. */
  void consumeH265(Stream& stream, const uint8_t* data, size_t size, int64_t timeUs);

  /** Consumes the body of an AAC PES packet, which is a chain of ADTS frames. */
  void consumeAdts(Stream& stream, const uint8_t* data, size_t size, int64_t timeUs);

  /** Consumes the body of an MPEG audio PES packet. */
  void consumeMpegAudio(Stream& stream, const uint8_t* data, size_t size, int64_t timeUs);

  /** Publishes the format of an H.264 track once its parameter sets are known. */
  bool publishH264Format(Stream& stream);

  /** Publishes the format of an H.265 track once its parameter sets are known. */
  bool publishH265Format(Stream& stream);

  /** Delivers one cc_data sample to the caption track derived from @p stream. */
  void emitCaptionSample(Stream& stream, const std::vector<uint8_t>& ccData, int64_t timeUs);

  /** Delivers one CEA-608 byte-pair sample to the 608 caption track of @p stream. */
  void emitCaption608Sample(Stream& stream, const std::vector<uint8_t>& bytePairs, int64_t timeUs);

  /** Creates the CEA-608 and CEA-708 caption tracks of @p stream if they do not exist yet. */
  void ensureCaptionTrack(Stream& stream);

  /** Creates the CEA-608 caption track of @p stream if it does not exist yet. */
  void ensureCaption608Track(Stream& stream);

  /** Reports a sample to a track and updates the duration and the seek index. */
  void emitSample(Stream& stream, const uint8_t* data, size_t size, int64_t timeUs, uint32_t flags);

  /** Reports a sample whose payload is already a block buffer to a track. */
  void emitSampleBuffer(Stream& stream, std::shared_ptr<AVBlockBuffer> buffer, size_t size, int64_t timeUs,
    uint32_t flags);

  /** Delivers a sample, applying the start of a seek that has no index entry. */
  void deliverSample(Stream& stream, const std::shared_ptr<AVBlockBuffer>& buffer, size_t size, int64_t timeUs,
    uint32_t flags);

  /** Appends a seek point for a key frame at the start of the current PES packet. */
  void addSeekPoint(Stream& stream, int64_t timeUs);

  /** Adjusts a timestamp, in 90 kHz units, so that the stream starts at zero. */
  int64_t adjustTimestamp(int64_t timestamp) const;

  /** Resets the parser state, keeping the tracks, after a seek. */
  void resetParserState(bool resetTimestampAdjuster, int64_t timeUs);

  /** Reports the end of every track and publishes the duration. */
  void finish(AVExtractorInput& input);

  AVExtractorOutput* output_ = nullptr;
  std::shared_ptr<AVTsSeekMap> seekMap_;
  std::map<int, Stream> streams_;
  std::vector<int> pmtPids_;
  std::set<int> knownStreamPids_;
  int pmtPid_ = -1;
  bool tracksEnded_ = false;
  bool tracksFinished_ = false;
  bool pendingSeekToStart_ = false;
  bool seenFormat_ = false;
  long position_ = 0;
  int pcrPid_ = -1;
  std::string sniffFailureReason_;

  /** The transport packet buffer. */
  std::vector<uint8_t> buffer_;
  std::vector<uint8_t> readBuffer_;
  size_t bufferStart_ = 0;
  size_t bufferEnd_ = 0;
  long bufferStartPosition_ = 0;

  /** The timestamp adjuster state. */
  mutable int64_t firstSampleTimestampUs_ = 0;
  mutable int64_t timestampOffsetUs_ = Definitions::kTimeUnset;
  mutable int64_t lastUnadjustedUs_ = Definitions::kTimeUnset;

  int64_t maxSampleTimeUs_ = Definitions::kTimeUnset;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_CONTAINER_TS_EXTRACTOR_H
