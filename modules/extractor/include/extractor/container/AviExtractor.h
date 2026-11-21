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

#ifndef CORAL_EXTRACTOR_CONTAINER_AVI_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_AVI_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/ExtractorInput.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace coral {

/**
 * Reads the chunks that hold the samples of one stream of an AVI file.
 *
 * <p>The class is incomplete on purpose: its implementation lives in the source file of the
 * extractor, so that the layout of the chunks of the container is not part of the public interface
 * of the module.
 */
class AVChunkReader;

/**
 * Extracts the tracks, the samples and the seek map of an AVI file, i.e. of a RIFF file whose form
 * type is {@code AVI }.
 *
 * <p>The extractor reads the {@code hdrl} list that describes the streams of the file, creates one
 * track per stream whose format it can describe, and then reads the chunks of the {@code movi} list
 * that follow, in the order in which the file stores them. Every chunk of a supported stream becomes
 * one {@link AVSample}. The {@code idx1} index is read from the end of the file when the input is
 * seekable; it supplies the key frame flags, the sample offsets and the seek map. Without an index
 * the timestamps are interpolated from the duration and the number of chunks of the stream header,
 * and the seek map reports the duration without seek points. The supported codecs are:
 * <ul>
 *   <li>audio: uncompressed PCM, MPEG audio, AAC, AC-3 and DTS;</li>
 *   <li>video: MPEG-4 part 2 (the {@code DIVX}, {@code DX50}, {@code FMP4} and {@code XVID} four
 *       character codes), H.264, and Motion JPEG.</li>
 * </ul>
 * A stream whose codec is not described by its format chunk is skipped rather than exposed with a
 * made up format, and so is a stream that carries neither audio nor video, e.g. a subtitle stream.
 *
 * <p>The extractor walks the chunks after the {@code hdrl} list to find the {@code idx1} index,
 * which is stored after the samples it describes. A seekable input is moved to the index and back
 * to the first chunk of the {@code movi} list; an input that cannot seek keeps the old forward-only
 * behaviour and publishes an unseekable seek map. The number of chunks of a stream, when no index is
 * available, is the length that its stream header declares.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AviExtractor}, together with the
 * {@code ChunkReader}, {@code AviChunk}, {@code ListChunk}, {@code AviMainHeaderChunk},
 * {@code AviStreamHeaderChunk}, {@code StreamFormatChunk} and {@code StreamNameChunk} helpers that
 * the Java extractor delegates to.
 */
class AVAviExtractor final : public AVExtractor {
public:
  AVAviExtractor() noexcept;

  ~AVAviExtractor() override;

  AVAviExtractor(const AVAviExtractor& other) = delete;

  AVAviExtractor& operator=(const AVAviExtractor& other) = delete;

  /**
   * Returns {@code kSniffResultCertain} when the input starts with the {@code RIFF} header of an
   * {@code AVI } file, and {@code kSniffResultNone} otherwise. The peek position of the input is
   * restored before returning.
   *
   * <p>The counterpart of {@code AviExtractor.sniff}.
   */
  int sniff(AVExtractorInput& input) override;

  /**
   * Stores the output that receives the tracks, the formats and the seek map, and resets the parse.
   * The counterpart of {@code AviExtractor.init}.
   */
  void init(AVExtractorOutput& output) override;

  /**
   * Parses the headers of the container and delivers one chunk of the {@code movi} list per call.
   * The counterpart of {@code AviExtractor.read}.
   */
  Result read(AVExtractorInput& input) override;

  /**
   * Restarts the container at the position of the seek. A position that the {@code idx1} index
   * describes starts at that chunk; a position of zero restarts the header parse.
   */
  bool seek(long position, int64_t timeUs) override;

  /**
   * Returns the position last observed on the input.
   */
  [[nodiscard]] long getPosition() const override { return position_; }

  /**
   * Returns the name of the container, for diagnostics.
   */
  [[nodiscard]] std::string getName() const override { return "AVI"; }

  /**
   * Returns why {@link #sniff} rejected the input, for diagnostics.
   */
  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * The state of the parser. This mirrors the {@code STATE_*} constants of {@code AviExtractor}.
   */
  enum class State {
    /** The header of the RIFF form is skipped. */
    kSkippingToHdrl,
    /** The header of the {@code hdrl} list is read. */
    kReadingHdrlHeader,
    /** The body of the {@code hdrl} list is buffered and parsed. */
    kReadingHdrlBody,
    /** The chunks of the file are walked until the {@code movi} list is found. */
    kFindingMoviHeader,
    /** The chunks of the {@code movi} list are read. */
    kReadingSamples,
    /** The end of the input was reached. */
    kEndOfInput,
  };

  /**
   * Parses the body of the {@code hdrl} list, creates the track of every supported stream and
   * publishes the duration of the container. Mirrors {@code AviExtractor.parseHdrlBody} and the
   * {@code ListChunk} parser it uses.
   */
  void parseHdrlBody(const AVDataBuffer& data);

  /**
   * Parses one {@code strl} list, i.e. one stream of the container, and creates its track. Mirrors
   * {@code AviExtractor.processStreamList}.
   *
   * @param streamId The identifier of the stream, which the chunk identifiers of the stream are
   *                 derived from.
   */
  void parseStreamList(const AVDataBuffer& data, size_t start, size_t end, int streamId);

  /**
   * Performs one transition of the parser. The counterpart of the switch that
   * {@code AviExtractor.read} performs.
   */
  Result readStep(AVExtractorInput& input);

  /**
   * Walks the chunks after the {@code hdrl} list and reads the {@code idx1} index when the input is
   * seekable. The index is stored after the samples of the row, so the input is moved forward to it
   * and back to the first chunk of the {@code movi} list.
   */
  bool scanIndex(AVExtractorInput& input);

  /** Builds the sample index of the OpenDML {@code indx} elements that were read from the header. */
  bool buildOpenDmlIndex(AVExtractorInput& input, long moviListStart, long moviDataStart);

  /**
   * Returns the reader of the stream that handles the given chunk identifier, or null when no stream
   * of the container does. Mirrors {@code AviExtractor.getChunkReader}.
   */
  [[nodiscard]] AVChunkReader* findChunkReader(uint32_t chunkId) const;

  /**
   * Reads the chunks of the {@code movi} list. Mirrors {@code AviExtractor.readMoviChunks}.
   */
  Result readMoviChunks(AVExtractorInput& input);

  /**
   * Returns whether one of the streams delivered a sample since the last seek, i.e. whether the
   * target of a seek that is being resolved was reached.
   */
  [[nodiscard]] bool hasDeliveredSample() const;

  /**
   * Reports the end of the container: the tracks are ended and a further read reports the end of the
   * input.
   */
  Result finish(AVExtractorInput& input);

  /**
   * The output that receives the tracks, the formats and the seek map. It is owned by the reader,
   * which outlives the extractor, therefore the extractor only observes it.
   */
  AVExtractorOutput* output_ = nullptr;
  /** The state of the parser. */
  State state_ = State::kSkippingToHdrl;
  /** The buffer that holds the body of the {@code hdrl} list while it is parsed. */
  AVDataBuffer hdrlBody_;
  /** A scratch buffer for the headers of the chunks that are read from the input. */
  AVDataBuffer scratch_;
  /** The readers of the streams of the container, in the order of their stream identifiers. */
  std::vector<std::unique_ptr<AVChunkReader>> chunkReaders_;
  /** The reader of the chunk that is being read, or null when no chunk is pending. */
  AVChunkReader* currentChunkReader_ = nullptr;
  /** The size of the body of the {@code hdrl} list, in bytes. */
  long hdrlBodySize_ = 0;
  /** The position of the first chunk of the {@code movi} list, or -1 when it is not known. */
  long moviStart_ = -1;
  /** The position of the first sample chunk, after the header of the {@code movi} list. */
  long moviDataStart_ = -1;
  /** The position of the end of the {@code movi} list, or -1 when it is not known. */
  long moviEnd_ = -1;
  /** An entry of the {@code idx1} index. */
  struct IndexedChunk {
    AVChunkReader* reader = nullptr;
    int sampleIndex = 0;
    int64_t timeUs = 0;
    long position = 0;
    bool keyFrame = false;
  };
  /** The chunks of the index, in the order in which the file stores them. */
  std::vector<IndexedChunk> indexedChunks_;
  /** The index entry of every sample chunk, by its absolute position. */
  std::unordered_map<long, IndexedChunk> indexedChunkByPosition_;
  /** An OpenDML {@code indx} element, kept until the stream readers exist. */
  struct OpenDmlIndex {
    uint32_t chunkId = 0;
    uint8_t indexType = 0;
    uint64_t baseOffset = 0;
    std::vector<uint32_t> entryOffsets;
    std::vector<uint32_t> entrySizes;
    std::vector<uint64_t> subIndexOffsets;
    std::vector<uint32_t> subIndexSizes;
  };
  /** The OpenDML indexes of the streams, read from the {@code hdrl} list. */
  std::vector<OpenDmlIndex> openDmlIndexes_;
  /** Whether the {@code idx1} index was read and can be used. */
  bool indexAvailable_ = false;
  /** The duration of the container in microseconds, or {@link Definitions::kTimeUnset}. */
  int64_t durationUs_ = Definitions::kTimeUnset;
  /**
   * The time that the samples must reach, i.e. the target of the seek that is being resolved, or -1
   * when no samples are skipped.
   */
  int64_t skipBeforeTimeUs_ = -1;
  /** Whether the body of the {@code hdrl} list was parsed. */
  bool headersParsed_ = false;
  /** Whether the seek map of the container was published. */
  bool seekMapPublished_ = false;
  /** Whether the tracks of the container were ended. */
  bool trackEnded_ = false;
  /** The position last observed on the input. */
  long position_ = 0;
  /** The reason why {@link #sniff} rejected the input. */
  std::string sniffFailureReason_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_CONTAINER_AVI_EXTRACTOR_H
