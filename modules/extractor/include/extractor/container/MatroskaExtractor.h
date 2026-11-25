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

#ifndef CORAL_EXTRACTOR_CONTAINER_MATROSKA_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_MATROSKA_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/Utility.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/**
 * Extracts the tracks and the samples of a Matroska or WebM container, as defined by the EBML
 * specification and by the Matroska and WebM container specifications.
 *
 * <p>The extractor reads the {@code EBML} header, walks the children of the {@code Segment} to
 * collect the {@code Info}, {@code Tracks} and {@code Cues} elements, and then reads the
 * {@code Cluster} elements and delivers one sample per frame of a {@code SimpleBlock} or of a
 * {@code BlockGroup}. All four lacing modes of the specification are supported.
 *
 * <p>A track whose codec is not recognized is skipped: it is neither reported to the
 * {@link AVExtractorOutput} nor buffered, its blocks are discarded. The {@code Tags},
 * {@code Chapters} and {@code Attachments} elements are skipped, and the encryption, the
 * {@code BlockAdditions} and the {@code ContentEncodings} layers are not supported.
 *
 * <p>An instance is driven by a single thread at a time, as required by {@link AVExtractor}, and it
 * never allocates a buffer whose size is derived from an unchecked element of the container.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code MatroskaExtractor}.
 */
class AVMatroskaExtractor final : public AVExtractor {
public:
  AVMatroskaExtractor() = default;

  ~AVMatroskaExtractor() override = default;

  AVMatroskaExtractor(const AVMatroskaExtractor& other) = delete;

  AVMatroskaExtractor& operator=(const AVMatroskaExtractor& other) = delete;

  /**
   * Peeks the four byte {@code EBML} header of the container.
   *
   * <p>Returns {@link AVExtractorsFactory::kSniffResultCertain} when the input starts with
   * {@code 0x1A45DFA3}, {@link AVExtractorsFactory::kSniffResultPossible} when its first two bytes
   * are {@code 0x1A 0x45}, and {@link AVExtractorsFactory::kSniffResultNone} otherwise. The peek
   * position is restored on return.
   *
   * <p>This is the counterpart of {@code MatroskaExtractor.sniff}.
   */
  int sniff(AVExtractorInput& input) override;

  /**
   * Scans the header elements of the container, creates one track per supported {@code TrackEntry},
   * and declares the seek map.
   *
   * <p>The {@code Info::Duration} element is read even when it follows the {@code Tracks} element,
   * which is the usual layout, because the children of the {@code Segment} are scanned ahead of the
   * first {@code Cluster}.
   *
   * <p>This is the counterpart of {@code MatroskaExtractor.init}.
   */
  void init(AVExtractorOutput& output) override;

  /**
   * Parses the next element of the container and delivers the samples of the blocks.
   *
   * <p>This is the counterpart of {@code MatroskaExtractor.read}.
   */
  Result read(AVExtractorInput& input) override;

  /**
   * Resets the parser state so that the reader may restart the parse at {@code position}.
   *
   * <p>The parse restarts at the {@code Segment} element of the container, or at the {@code EBML}
   * header when the requested position is the start of the input.
   *
   * <p>This is the counterpart of {@code MatroskaExtractor.seek}.
   */
  bool seek(long position, int64_t timeUs) override;

  /**
   * Returns the position of the element that the parser processed last.
   */
  [[nodiscard]] long getPosition() const override;

  [[nodiscard]] std::string getName() const override { return "Matroska"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * The element of the container that the parser is reading.
   */
  enum class Phase {
    /** Nothing was read yet, the tracks and the seek map are not declared. */
    kInitialize,
    /** The {@code EBML} header is being read. */
    kReadEbmlHeader,
    /** The {@code Segment} element is being located. */
    kReadSegment,
    /** The restart position of the {@code Segment} is being located after a seek. */
    kScanSegment,
    /** The children of the {@code Segment} are scanned. */
    kScanHeaders,
    /** The clusters are scanned to collect the seek points. */
    kScanClusterOrder,
    /** The cues are resolved to a seek map. */
    kFillSeekMap,
    /** The clusters are read, and the samples are delivered. */
    kReadClusters,
    /** The remaining bytes of the input are discarded. */
    kSkipContent,
  };

  /**
   * The codec delay and the seek pre-roll of a track, both in nanoseconds.
   */
  struct CodecDelay {
    /** The {@code CodecDelay} element. */
    int64_t codecDelayNs;
    /** The {@code SeekPreRoll} element. */
    int64_t seekPreRollNs;
  };

  /**
   * The colour description of a video track.
   */
  struct Colour {
    /** The {@code MatrixCoefficients} element. */
    int matrixCoefficients;
    /** The {@code TransferCharacteristics} element. */
    int transferCharacteristics;
    /** The {@code Primaries} element. */
    int primaries;
    /** The {@code Range} element. */
    int range;
  };

  /**
   * The {@code Video} element of a {@code TrackEntry}.
   */
  struct VideoData {
    /** The {@code PixelWidth} element. */
    int pixelWidth;
    /** The {@code PixelHeight} element. */
    int pixelHeight;
    /** The {@code DisplayWidth} element. */
    int displayWidth;
    /** The {@code DisplayHeight} element. */
    int displayHeight;
    /** The {@code DisplayUnit} element. */
    int displayUnit;
    /** The {@code AlphaMode} element. */
    int alphaMode;
    /** The {@code FlagInterlaced} element. */
    int interlaced;
    /** The {@code StereoMode} element. */
    int stereoMode;
    /** The {@code ProjectionType} element. */
    int projectionType;
    /** The rotation, in degrees clockwise, described by the projection elements. */
    int rotationDegrees;
    /** The rows of the {@code ProjectionPrivate} transformation matrix. */
    float projectionMatrix[3][3];
    /** The colour description of the track. */
    Colour colour;
  };

  /**
   * The {@code Audio} element of a {@code TrackEntry}.
   */
  struct AudioData {
    /** The {@code SamplingFrequency} element, in Hz. */
    float samplingFrequency;
    /** The {@code OutputSamplingFrequency} element, in Hz. */
    float outputSamplingFrequency;
    /** The {@code Channels} element. */
    int channelCount;
    /** The {@code BitDepth} element. */
    int bitDepth;
  };

  /**
   * The description of one {@code TrackEntry} of the container.
   */
  struct Track {
    /** The identifier of the track, as passed to {@link AVExtractorOutput::track}. */
    int id;
    /** The {@code TrackNumber} element, which identifies the blocks of the track. */
    int64_t number;
    /** The {@code TrackUid} element, which the tag targets refer to. */
    int64_t uid = 0;
    /** The {@code TrackType} element: 1 video, 2 audio, 17 subtitle. */
    int type;
    /** The {@code CodecID} element. */
    std::string codecId;
    /** The {@code CodecPrivate} element, or the codec specific data that replaces it. */
    std::vector<uint8_t> codecPrivate;
    /**
     * The number of bytes of the length prefix of every NAL unit of a block, or 0 when the blocks of the
     * track do not frame their NAL units with a length prefix.
     *
     * <p>An AVC track declares it in the fifth byte of its {@code CodecPrivate} element and an HEVC one
     * in its 22nd: it is what tells the length prefixed framing of the container from the Annex-B framing
     * the decoders read.
     */
    int nalLengthSize = 0;
    /** The mime type of the samples. */
    std::string mimeType;
    /** The media type of the samples. */
    MediaType mediaType;
    /** The {@code DefaultDuration} element in nanoseconds, or 0 when the element is absent. */
    int64_t defaultDurationNs;
    /** The codec delay and the seek pre-roll of the track. */
    CodecDelay delay;
    /** The {@code Video} element. */
    VideoData video;
    /** The {@code Audio} element. */
    AudioData audio;
    /** The RFC 6381 codec string, when it can be derived from the codec private data. */
    std::string codecs;
    /** Whether the track is a subtitle track. */
    bool isSubtitle;
    /** The {@code Name} element (the human readable track label), or empty. */
    std::string name;
    /** The {@code Language} element (ISO 639-2) of the track, or empty. */
    std::string language;
    /** The {@code LanguageIETF} element (BCP 47) of the track, or empty. */
    std::string languageIetf;
    /** The {@code FlagDefault} element: whether the track is selected when the user did not choose. */
    bool flagDefault = true;
    /** The {@code FlagForced} element: whether the subtitles must be shown. */
    bool flagForced = false;
    /** Whether {@code kFlagFirstSample} was already reported for the track. */
    bool hasFirstSample;
    /** The last duration that was reported for the track, in microseconds. */
    int64_t lastDurationUs;
    /** The output that receives the samples of the track. */
    std::shared_ptr<AVTrackOutput> output;
    /** The format that was published for the track, or null before the first sample. */
    std::shared_ptr<MediaFormat> publishedFormat;
  };

  /**
   * A cue resolved for one track: a time and a byte position.
   */
  struct SeekEntry {
    /** The microsecond time of the cue. */
    int64_t timeUs;
    /** The byte position of the cluster of the cue. */
    long position;
  };

  /**
   * The timecode and the position of a {@code Cluster}.
   */
  struct ClusterPosition {
    /** The {@code Cluster::Timecode} element, in timecode scale units. */
    int64_t timecode;
    /** The byte position of the start of the cluster. */
    long position;

    bool operator<(const ClusterPosition& other) const { return timecode < other.timecode; }
  };

  // ================================================================================================
  // EBML helpers
  // ================================================================================================

  /**
   * Reads an EBML variable length integer from a buffer, keeping its length descriptor bits.
   *
   * @return The identifier, or -1 when the buffer is exhausted.
   */
  static int64_t parseVint(AVDataBuffer& data);

  /**
   * Reads an EBML variable length integer from a buffer, without its length descriptor bits.
   *
   * @return The size, or -2 when the element has an unknown size, or -1 on failure.
   */
  static int64_t parseSize(AVDataBuffer& data);

  /**
   * Reads a signed EBML variable length integer from a buffer, as used by the lacing of a block.
   *
   * @return The value, or -1 on failure.
   */
  static int64_t parseSignedVint(AVDataBuffer& data);

  /**
   * Reads an unsigned big endian integer of at most eight bytes.
   */
  static int64_t readUnsigned(const uint8_t data[], size_t size);

  /**
   * Reads a value of at most 32 bits from a bit reader, or 0 when it is not available.
   */
  static int readBits(AVBitReader& bits, int count);

  /**
   * Reads an unsigned Exp-Golomb code from a bit reader.
   */
  static int readUe(AVBitReader& bits);

  /**
   * Reads a variable length integer from the input, byte per byte, without moving the read
   * position.
   *
   * @param keepMarker Whether the length descriptor bits are kept, which is the case of an element
   *                   identifier.
   * @param outLength When not null, receives the number of bytes the value spans, which is what a caller
   *                  passes as {@code startOffset} to read the value that follows it.
   * @param startOffset The number of bytes of the peeked data to skip before the value, e.g. the bytes
   *                    of an element identifier when its size is peeked.
   *
   * @return The value, or -2 when the size of an element is unknown, or -1 on failure.
   */
  static int64_t peekVint(AVExtractorInput& input, bool keepMarker, long* outLength = nullptr,
                          long startOffset = 0);

  /**
   * Places the read position at the next element of the current parent, and reads its identifier
   * and its size.
   *
   * @param outSize The size of the element, or -2 when its size is unknown.
   *
   * @return {@code true} on success, and {@code false} at the end of the input or when the element
   *         crosses the end of the current parent.
   */
  bool readElementHeader(AVExtractorInput& input, long& outId, long& outSize);

  /**
   * Reads the payload of the current element into the given buffer.
   */
  bool takeElement(AVExtractorInput& input, long size, AVDataBuffer& target) const;

  /**
   * Moves the read position past the current element, and pops its frame.
   */
  bool advanceElement(AVExtractorInput& input);

  /**
   * Returns the end of the parent of the current element, in bytes.
   */
  [[nodiscard]] long parentFrameEnd() const;

  /**
   * Returns the expected end position of the current element in the input.
   */
  [[nodiscard]] long currentElementEnd() const;

  /**
   * Pushes the description of the current element on the stack, and returns the number of bytes
   * that it occupies in the input, or -1 when it is empty.
   */
  long pushElementFrame(AVExtractorInput& input, long size);

  /**
   * Pops the description of the current element.
   */
  void popElementFrame();

  /**
   * Buffers the payload of a compound element that is about to be parsed.
   *
   * @return The number of bytes that were buffered, or -1 when the element is too large.
   */
  long loadElement(AVExtractorInput& input, long size);

  /**
   * Discards the payload of the current element and pops its frame.
   *
   * @return {@code false} when the input ended before the element was consumed.
   */
  bool skipElement(AVExtractorInput& input);

  /**
   * Returns whether the parser reached the end of the element it is reading.
   */
  [[nodiscard]] bool isEndOfParent(AVExtractorInput& input) const;

  // ================================================================================================
  // Header parsing
  // ================================================================================================

  /**
   * Resets the state of the parser before a seek.
   */
  void resetParser();

  /**
   * Reads the {@code EBML} header, and sets {@link #webm_}.
   */
  bool readEbmlHeader(AVExtractorInput& input);

  /**
   * Locates the {@code Segment} element of the container.
   *
   * @return One of the {@link Result} constants.
   */
  Result readSegment(AVExtractorInput& input);

  /**
   * Scans the children of the {@code Segment} until the {@code Cluster}s.
   *
   * @return One of the {@link Result} constants.
   */
  Result scanHeaders(AVExtractorInput& input);

  /**
   * Scans the clusters of the container and records their timecodes and their positions.
   *
   * @return One of the {@link Result} constants.
   */
  Result scanClusterOrder(AVExtractorInput& input);

  /**
   * Finds the first byte of the {@code Segment} element at or after the current position.
   */
  bool scanForSegment(AVExtractorInput& input);

  /**
   * Returns whether the given identifier opens one of the children of the {@code Segment}.
   */
  static bool isSegmentChild(long id);

  /**
   * Parses the children of an {@code Info} element.
   */
  Result parseSegmentInfo(AVExtractorInput& input);

  /**
   * Parses the children of a {@code Tracks} element, and reports the supported tracks.
   */
  Result parseTracks(AVExtractorInput& input);

  /**
   * Returns the given frame of a block in the Annex-B framing the decoders read, or a copy of the frame
   * when the track does not frame its NAL units with a length prefix.
   *
   * <p>The parameter sets of an AVC track are prepended to a key frame, as the {@code h264_mp4toannexb}
   * filter of the platform extractor does: a decoder that starts reading at a key frame is given them in
   * band.
   */
  [[nodiscard]] std::shared_ptr<AVBlockBuffer> toAnnexB(const uint8_t* frame, size_t frameSize,
                                                        const Track& track) const;

  /**
   * Builds the payload of a subtitle block.
   *
   * <p>A block of a subtitle track states only its text; the timing and, for SSA, the fields that
   * precede the text are what the container states once. The returned payload is the document that
   * the parser of the format reads, i.e. a SubRip cue, a WebVTT document or an SSA dialogue line,
   * whose timing is relative to the block.
   *
   * @param frame The text of the block.
   * @param frameSize The number of bytes of the text.
   * @param durationUs The duration of the block, in microseconds.
   * @param track The track the block belongs to.
   */
  [[nodiscard]] static std::shared_ptr<AVBlockBuffer>
  buildSubtitleSample(const uint8_t* frame, size_t frameSize, int64_t durationUs,
                      const Track& track);

  /**
   * Parses a {@code TrackEntry} element, which must be buffered in {@code element_}.
   *
   * @return {@code true} when the track is supported and was reported.
   */
  bool parseTrackEntry();

  /**
   * Parses the children of a {@code Video} element.
   */
  void parseVideo(VideoData& video);

  /**
   * Parses the children of an {@code Audio} element.
   */
  void parseAudio(AudioData& audio);

  /**
   * Parses the children of a {@code Projection} element.
   */
  void parseProjection(VideoData& video);

  /**
   * Parses the children of a {@code Colour} element.
   */
  void parseColour(Colour& colour);

  /**
   * Applies a {@code TrackEntry} field that carries a plain integer.
   *
   * @return {@code true} when the identifier is one of those fields.
   */
  static bool applyTrackInteger(Track& track, long id, int64_t value);

  /**
   * Derives the mime type, the media type and the codec specific data of a track from its
   * {@code CodecID} and its {@code CodecPrivate} elements.
   *
   * @return {@code true} when the codec is supported.
   */
  static bool resolveCodec(Track& track);

  /**
   * Builds the {@link MediaFormat} of a track.
   */
  /**
   * Parses a {@code Tags} element, i.e. the {@code Tag} elements and their {@code SimpleTag} lists,
   * and appends their entries to the metadata of the movie or of the targeted track.
   */
  void parseTagsElement(AVDataBuffer& data);

  /**
   * Parses a {@code Chapters} element, i.e. the {@code EditionEntry} and {@code ChapterAtom}
   * elements, and appends their entries to the metadata of the movie.
   */
  void parseChaptersElement(AVDataBuffer& data);

  /**
   * Publishes the metadata that the tags and the chapters declared to the tracks whose format was
   * already published, because both usually follow the clusters of the segment.
   */
  void publishMetadata();

  std::shared_ptr<MediaFormat> buildFormat(const Track& track) const;

  /**
   * Parses a {@code V_MPEG4/ISO/AVC} codec private data, i.e. an AVCDecoderConfigurationRecord.
   */
  static bool parseAvcCodecPrivate(const uint8_t* data, size_t size, int& outWidth, int& outHeight,
                                   std::vector<uint8_t>& outCsd);

  /**
   * Parses a {@code V_MPEGH/ISO/HEVC} codec private data, i.e. an HEVCDecoderConfigurationRecord.
   */
  static bool parseHevcCodecPrivate(const uint8_t* data, size_t size, int& outWidth, int& outHeight,
                                    std::string& outCodecs);

  /**
   * Parses an {@code AV1CodecConfigurationRecord} and derives its RFC 6381 codec string.
   */
  static bool parseAv1CodecPrivate(const uint8_t* data, size_t size, std::string& outCodecs);

  /**
   * Splits a {@code CodecPrivate} of a Vorbis track into its three headers.
   */
  static bool parseVorbisCodecPrivate(const uint8_t* data, size_t size, Track& track);

  /**
   * Reads the width and the height of an H.264 sequence parameter set.
   */
  static bool parseAvcSps(const std::vector<uint8_t>& sps, int& outWidth, int& outHeight);

  /**
   * Reads the width and the height of an H.265 sequence parameter set.
   */
  static bool parseHevcSps(const std::vector<uint8_t>& sps, int& outWidth, int& outHeight);

  /**
   * Returns whether the buffer holds a valid UTF-8 string. A subtitle sample that is not valid
   * UTF-8 is dropped rather than delivered.
   */
  static bool isValidUtf8(const uint8_t* data, size_t size);

  // ================================================================================================
  // Cluster parsing
  // ================================================================================================

  /**
   * Parses the children of a {@code Cues} element, and buffers the cues in {@code rawCues_}.
   */
  Result parseCues(AVExtractorInput& input);

  /**
   * Parses the children of a {@code CuePoint} element.
   */
  void parseCuePoint();

  /**
   * Parses the children of a {@code CueTrackPositions} element.
   */
  void parseCueTrackPositions();

  /**
   * Builds the seek map from the cues that were collected, and declares it.
   *
   * @return One of the {@link Result} constants.
   */
  Result fillSeekMap();

  /**
   * Reads the children of a {@code Cluster} element.
   *
   * @return One of the {@link Result} constants.
   */
  Result readCluster(AVExtractorInput& input);

  /**
   * Reads a {@code BlockGroup} element and delivers the samples of its {@code Block}.
   */
  Result readBlockGroup(AVExtractorInput& input);

  /**
   * Reads a {@code Block} or a {@code SimpleBlock} element, and delivers its samples.
   *
   * @param keyFrame Whether the block belongs to a {@code BlockGroup} without a
   *                 {@code ReferenceBlock}, which makes every frame a key frame.
   */
  Result readBlock(AVExtractorInput& input, const Track& track, bool keyFrame);

  /**
   * Parses a buffered block, and records the frame sizes and the offset of the first frame.
   *
   * @return {@code true} when the block is well formed.
   */
  bool parseBlockData();

  /**
   * Delivers the frames of the current block, which must be buffered in {@code currentBlock_}.
   */
  void deliverBlock();

  /**
   * Delivers one frame of the current block.
   *
   * @param frameOffset The offset of the frame in the block buffer.
   * @param frameSize The size of the frame in bytes.
   * @param frameIndex The index of the frame inside the block.
   * @param track The track the block belongs to.
   */
  void deliverFrame(size_t frameOffset, size_t frameSize, size_t frameIndex, Track& track);

  /**
   * Returns the time in microseconds of a timecode expressed in timecode scale units.
   */
  [[nodiscard]] int64_t scaleMicros(int64_t timecode) const;

  /**
   * Discards the remaining bytes of the input.
   */
  static Result skipContent(AVExtractorInput& input);

  /**
   * Returns the track with the given {@code TrackNumber}, or null.
   */
  [[nodiscard]] Track* findTrack(int64_t number);

  /**
   * Returns the byte position of the cue that is the closest to the given time, or -1.
   */
  [[nodiscard]] long findCuePosition(int64_t timeUs) const;

  /**
   * Returns the byte position of the cluster that is the closest to the given time, or -1.
   */
  [[nodiscard]] long findClusterPosition(int64_t timeUs) const;

  // ================================================================================================
  // State
  // ================================================================================================

  /** The output that receives the tracks and the seek map. */
  AVExtractorOutput* output_ = nullptr;
  /** The description of why {@link #sniff} rejected the input. */
  std::string sniffFailureReason_;

  /** The element that the parser is reading. */
  Phase phase_ = Phase::kInitialize;
  /** Whether the container is a WebM container rather than a Matroska container. */
  bool webm_ = false;
  /** Whether the tracks and the seek map were already declared. */
  bool headersDeclared_ = false;

  /** Whether a seek must be resolved before the next element is parsed. */
  bool pendingSeek_ = false;
  /** The byte position of the pending seek. */
  long pendingSeekPosition_ = -1;
  /** The time of the pending seek, in microseconds, or -1. */
  int64_t pendingSeekTimeUs_ = -1;
  /** The position of the {@code Cluster} that the parser must resume at, or -1. */
  long pendingClusterPosition_ = -1;

  /** The byte position of the {@code Segment} element, or -1. */
  long segmentStartPosition_ = -1;
  /** The position of the end of the current element, in bytes, or -1. */
  long elementEnd_ = -1;
  /** The last position that the parser reached in the input. */
  long parsePosition_ = 0;
  /** The position of the end of the input. */
  long inputEnd_ = 0;
  /** Whether the frame of the cluster that is being read was pushed by the parser. */
  bool clusterFramePushed_ = false;

  /** The {@code Info::TimecodeScale} element, in nanoseconds. */
  int64_t timecodeScale_ = 1000000;
  /** The {@code Info::Duration} element, in timecode scale units, or -1. */
  int64_t durationTicks_ = -1;
  /** The {@code Info::Title} element. */
  std::string title_;

  /** The metadata that the {@code Tags} and {@code Chapters} elements declared. */
  std::vector<std::shared_ptr<Metadata::Entry>> globalMetadata_;
  /** The metadata of the tracks that a {@code Tag} targeted, keyed by {@code TrackUid}. */
  std::map<int64_t, std::vector<std::shared_ptr<Metadata::Entry>>> trackMetadata_;
  /** Whether the {@code Tags} element was parsed, so that seeking does not parse it twice. */
  bool tagsParsed_ = false;
  /** Whether the {@code Chapters} element was parsed, so that seeking does not parse it twice. */
  bool chaptersParsed_ = false;

  /** The tracks that were reported to the output. */
  std::vector<Track> tracks_;

  /** The timecode of the current cluster, in timecode scale units. */
  int64_t clusterTimecode_ = 0;
  /** The start position of the current cluster, in bytes. */
  long clusterStartPosition_ = -1;
  /** The position and the timecode of the clusters that were scanned. */
  std::vector<ClusterPosition> clusterOrder_;

  /** The cues of the {@code Cues} element, before they are grouped per track. */
  std::vector<std::pair<int64_t, SeekEntry>> rawCues_;
  /** The cues collected for the {@code CuePoint} that is being parsed. */
  std::vector<std::pair<int64_t, SeekEntry>> pendingCues_;
  /** The track number of the {@code CueTrackPositions} that is being parsed. */
  int64_t pendingCueTrackNumber_ = 0;
  /** The cluster position of the {@code CueTrackPositions} that is being parsed. */
  int64_t pendingCueClusterPosition_ = 0;

  /** The buffer that holds the block that is being parsed. */
  AVDataBuffer* currentBlock_ = nullptr;
  /** The track of the block that is being parsed, or null. */
  Track* currentTrack_ = nullptr;
  /** The timecode of the block relative to its cluster, in timecode scale units. */
  int64_t blockTick_ = 0;
  /** Whether the frames of the current block are key frames. */
  bool blockKeyFrame_ = false;
  /** The number of frames of the block. */
  size_t blockFrameCount_ = 1;
  /** The offset of the first frame of the block in the block buffer. */
  size_t blockContentStart_ = 0;
  /** The size of every frame of the block. */
  std::vector<size_t> blockFrameSizes_;
  /** The {@code BlockDuration} of the current block, in timecode scale units, or -1. */
  int64_t blockDurationTicks_ = -1;

  /** The payload of the element that is being parsed. */
  AVDataBuffer element_;
  /** The payload of a block that does not fit in {@link #element_}. */
  AVDataBuffer blockData_;
  /** The buffer used to discard the payload of the elements that are not needed. */
  AVDataBuffer scratch_;
  /** The stack of the elements that are being parsed. */
  std::vector<std::pair<long, long>> elementFrames_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_CONTAINER_MATROSKA_EXTRACTOR_H
