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

#ifndef CORAL_EXTRACTOR_CONTAINER_MP4_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_MP4_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/ExtractorInput.h>

#include <media/ColorInfo.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Extracts the tracks, the samples and the seek map of an ISO base media file, i.e. of an MP4,
 * QuickTime (MOV), 3GPP or fragmented MP4 container.
 *
 * <p>The extractor parses the movie box when it reaches it, creates one {@link AVTrackOutput} per
 * track whose sample description it can describe, publishes the {@link MediaFormat} of every sample
 * description it may encounter, and then delivers the samples of the sample tables and of the movie
 * fragments that follow. The tracks, the seek map and the formats are only created once the
 * {@code moov} box was parsed, therefore {@link AVExtractorOutput#track} and
 * {@link AVExtractorOutput#seekMap} are called from {@link #read}, not from {@link #init}: the
 * movie box is only reachable through the input.
 *
 * <p>The supported codecs are:
 * <ul>
 *   <li>audio: {@code mp4a}, {@code ac-3}, {@code ec-3}, {@code ac-4}, {@code Opus}, {@code fLaC},
 *       {@code alac}, {@code sowt} and {@code twos};</li>
 *   <li>video: {@code avc1}, {@code avc3}, {@code hvc1}, {@code hev1}, {@code av01}, {@code vp08},
 *       {@code vp09}, {@code mp4v}, {@code s263}, {@code dvh1} and {@code dvhe};</li>
 *   <li>subtitle: {@code tx3g} timed text, {@code wvtt} WebVTT and {@code stpp} TTML, whose samples
 *       the parsers of {@code extractor/text} decode.</li>
 * </ul>
 * A track whose sample entry cannot be described, e.g. a video track without a configuration record,
 * is skipped rather than exposed with a made up format. Encryption is not
 * supported: {@code senc}, {@code saiz}, {@code saio} and {@code pssh} are treated as unknown boxes
 * and skipped, and no protection scheme is reported.
 *
 * <p>The extractor is driven by a single thread at a time, as documented by {@link AVExtractor}. It
 * only holds the buffers it parses with, therefore no destructor work is required beyond the
 * release of those buffers.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code Mp4Extractor}, together with
 * the {@code AtomParsers}, {@code Sniffer}, {@code Mp4SampleTable} and {@code BoxParser} helpers
 * that the Java extractor delegates the box parsing to.
 */
class AVMp4Extractor final : public AVExtractor {
public:
  AVMp4Extractor() = default;

  ~AVMp4Extractor() override = default;

  AVMp4Extractor(const AVMp4Extractor& other) = delete;

  AVMp4Extractor& operator=(const AVMp4Extractor& other) = delete;

  /**
   * Returns {@code kSniffResultCertain} when the first box is one of the ISO base media boxes,
   * {@code kSniffResultPossible} when one of them appears in the first
   * {@link AVExtractor#kSniffPeekLimit} bytes, and {@code kSniffResultNone} otherwise. The peek
   * position of the input is restored before returning.
   *
   * <p>The counterpart of ExoPlayer's {@code Mp4Extractor.sniff} and of its {@code Sniffer} helper.
   */
  int sniff(AVExtractorInput& input) override;

  /**
   * Stores the output that receives the tracks, the formats and the seek map. The counterpart of
   * {@code Mp4Extractor.init}.
   */
  void init(AVExtractorOutput& output) override;

  /**
   * Parses the boxes of the container and delivers one sample, or skips one bounded run of bytes,
   * per call. The counterpart of {@code Mp4Extractor.read}.
   */
  Result read(AVExtractorInput& input) override;

  /**
   * Resets the parser and moves the sample readers to the requested time, so that the next
   * {@link #read} resumes at the position the reader set on the input. The counterpart of
   * {@code Mp4Extractor.seek}, extended with the sample table resolution that the Java extractor
   * leaves to its {@code Mp4SampleTable}.
   */
  bool seek(long position, int64_t timeUs) override;

  /**
   * Returns the position last observed on the input. The counterpart of
   * {@code Mp4Extractor.getPosition}.
   */
  [[nodiscard]] long getPosition() const override { return position_; }

  /**
   * Returns why {@link #sniff} rejected the input. The counterpart of
   * {@code Mp4Extractor.getSniffFailureReason}.
   */
  [[nodiscard]] std::string getName() const override { return "MP4"; }

  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * The state of the box parser. This mirrors the {@code STATE_READING_ATOM_HEADER},
   * {@code STATE_READING_ATOM_PAYLOAD} and {@code STATE_READING_SAMPLE} constants of
   * {@code Mp4Extractor}, with the payload skipping split into its own state so that a single call
   * of {@link #read} never consumes an unbounded amount of input.
   */
  enum class State {
    /** The header of the next box is read. */
    kReadingAtomHeader,
    /** The payload of the current box is skipped, with a bounded number of bytes per call. */
    kSkippingAtomPayload,
    /** The payload of the current box is buffered so that it can be parsed. */
    kReadingAtomPayload,
    /** The samples described by the sample tables, or by the movie fragments, are delivered. */
    kReadingSample,
  };

  /**
   * The header of a box that lives in a buffered payload. Mirrors the header that
   * {@code BoxParser.readAtomHeader} reads.
   */
  struct BoxHeader {
    /** The four character code of the box. */
    uint32_t type = 0;
    /** The offset of the box payload in the buffer. */
    uint64_t payloadOffset = 0;
    /** The size of the box payload, in bytes. */
    uint64_t payloadSize = 0;
    /** The offset of the box that follows this one in the buffer. */
    uint64_t nextOffset = 0;
    /** Whether the header could be read, i.e. whether the box is well formed. */
    bool valid = false;
  };

  /**
   * One entry of an {@code stts} box. Mirrors {@code Mp4SttsAtom}.
   */
  struct TimeToSampleEntry {
    /** The number of consecutive samples that share the duration. */
    uint32_t sampleCount = 0;
    /** The duration of the samples, in the timescale of the track. */
    uint32_t sampleDelta = 0;
  };

  /**
   * One entry of a {@code ctts} box. Mirrors {@code Mp4CttsAtom}.
   */
  struct CompositionOffsetEntry {
    /** The number of consecutive samples that share the composition offset. */
    uint32_t sampleCount = 0;
    /** The composition offset of the samples, in the timescale of the track. */
    int32_t sampleOffset = 0;
  };

  /**
   * One entry of an {@code stsc} box. Mirrors {@code Mp4StscAtom}.
   */
  struct SampleToChunkEntry {
    /** The one based index of the first chunk the entry applies to. */
    uint32_t firstChunk = 1;
    /** The number of samples of every chunk the entry applies to. */
    uint32_t samplesPerChunk = 0;
    /** The one based index of the sample description the samples use. */
    uint32_t sampleDescriptionIndex = 1;
  };

  /**
   * One entry of an {@code elst} box. Mirrors the edit list arrays of {@code AtomParsers}.
   */
  struct EditListEntry {
    /** The duration of the edit, in the timescale of the movie. */
    int64_t segmentDuration = 0;
    /** The time of the first media sample of the edit, in the timescale of the track, or -1 when
     * the edit is empty, i.e. when it only delays the track. */
    int64_t mediaTime = -1;
    /** The rate of the edit, in 16.16 fixed point. */
    int32_t mediaRate = 0x00010000;
  };

  /**
   * The format of the samples that refer to one entry of an {@code stsd} box. Mirrors the
   * {@code Mp4AudioSampleEntry} and {@code Mp4VideoSampleEntry} boxes, and the {@code MediaFormat}
   * that {@code AtomParsers} builds out of them.
   */
  struct SampleDescription {
    /** Whether the codec of the entry can be described, i.e. whether the entry is supported. */
    bool supported = false;
    /** Whether the entry describes video rather than audio. */
    bool video = false;
    /** Whether the entry describes a subtitle track. */
    bool subtitle = false;
    /** Whether the entry describes a timed metadata track. */
    bool metadata = false;
    /** Whether the sample payload starts with a caption box (`cdat`/`ccdp`) to strip. */
    bool stripCaptionEnvelope = false;
    /** The language declared by a `clcp` box, or empty when the entry has none. */
    std::string captionLanguage;
    /** The caption service number declared by `clcp`, or 0 when unknown. */
    int captionServiceNumber = 0;
    /** Whether `clcp` marks the captions as easy to read. */
    bool captionEasyReader = false;
    /** Whether `clcp` marks the captions as authored for a wide aspect ratio. */
    bool captionWideAspect = false;
    /**
     * The offset to add to the timing that a subtitle sample states in its own data, or
     * {@link MediaFormat::OFFSET_SAMPLE_RELATIVE} when that timing is relative to the sample.
     */
    int64_t subsampleOffsetUs = 0;
    /** The MIME type of the samples, e.g. {@code video/avc}. */
    std::string mimeType;
    /** The RFC 6381 codecs string of the samples, e.g. {@code avc1.64001f}, or empty. */
    std::string codecs;
    /** The sample rate in Hz. */
    int sampleRate = 0;
    /** The number of audio channels. */
    int channelCount = 0;
    /** The width of the video, in pixels. */
    int width = 0;
    /** The height of the video, in pixels. */
    int height = 0;
    /** The profile of the codec, or 0 when unknown. */
    int profile = 0;
    /** The level of the codec, or 0 when unknown. */
    int level = 0;
    /** The average bitrate of the samples in bits per second, or 0 when unknown. */
    int averageBitrate = 0;
    /** The number of samples to trim from the start of the decoded stream. */
    int encoderDelay = 0;
    /** The number of samples to trim from the end of the decoded stream. */
    int encoderPadding = 0;
    /** The color/HDR information of a video sample entry. */
    ColorInfo colorInfo;
    /** The Dolby Vision profile, or 0 when the entry is not Dolby Vision. */
    int dolbyVisionProfile = 0;
    /** The Dolby Vision level. */
    int dolbyVisionLevel = 0;
    /** The codec specific data of the samples, in the framing the decoders expect. */
    std::vector<std::vector<uint8_t>> codecSpecificData;
    /**
     * The number of bytes of the length prefix of every NAL unit of a sample, or 0 when the samples of
     * the entry are not length prefixed.
     *
     * <p>An AVC configuration record declares it in its fifth byte and an HEVC one in its 22nd, and the
     * samples of the entry frame their NAL units with it: it is what tells the length prefixed framing
     * of the container from the Annex-B framing the decoders read.
     */
    int nalLengthSize = 0;
  };

  /**
   * A sample of a movie fragment, i.e. a sample that is not described by the sample tables of the
   * {@code moov} box. Mirrors the sample run that {@code Mp4TrunAtom} describes.
   */
  struct FragmentSample {
    /** The absolute position of the sample in the input. */
    uint64_t offset = 0;
    /** The size of the sample, in bytes. */
    uint32_t size = 0;
    /** The presentation timestamp of the sample, in microseconds. */
    int64_t timeUs = Definitions::kTimeUnset;
    /** A combination of the {@link AVSample::Flags} constants. */
    uint32_t flags = static_cast<uint32_t>(AVSample::kFlagNone);
    /** The one based index of the sample description of the sample. */
    uint32_t sampleDescriptionIndex = 1;
  };

  /**
   * The defaults that a {@code trex} box declares for a track of a fragmented movie. Mirrors
   * {@code Mp4TrexAtom}.
   */
  struct TrackExtendsDefaults {
    /** The default sample description index. */
    uint32_t sampleDescriptionIndex = 1;
    /** The default duration of a sample, in the timescale of the track. */
    uint32_t sampleDuration = 0;
    /** The default size of a sample, in bytes. */
    uint32_t sampleSize = 0;
    /** The default flags of a sample. */
    uint32_t sampleFlags = 0;
  };

  /**
   * A track, i.e. everything that the {@code trak} boxes and the movie fragments of one track
   * describe. Mirrors ExoPlayer's {@code Mp4Track} together with its {@code Mp4SampleTable}.
   */
  struct Track {
    /** The identifier of the track, as passed to {@link AVExtractorOutput#track}. */
    int index = Definitions::kIndexUnset;
    /** The type of the samples of the track. */
    MediaType mediaType = MediaType::kMediaTypeInvalid;
    /** The identifier of the track in the container. */
    uint32_t trackId = 0;    /** The number of timestamp units per second. */
    int64_t timescale = 0;
    /** The duration of the track in microseconds. */
    int64_t durationUs = Definitions::kTimeUnset;
    /** The ISO 639-2/T language of the track, or an empty string. */
    std::string language;
    /** The clockwise rotation of the video, in degrees. */
    int rotation = 0;
    /** The largest sample of the track, in bytes. */
    int maxInputSize = 0;
    /** The formats of the sample descriptions of the track. */
    std::vector<SampleDescription> sampleDescriptions;
    /** Whether at least one sample description is supported. */
    bool supported = false;
    /** The caption track derived from the SEI of a video track, declared with the video track. */
    std::shared_ptr<AVTrackOutput> captionOutput;

    /** The entries of the {@code stts} box. */
    std::vector<TimeToSampleEntry> timeToSample;
    /** The entries of the {@code ctts} box. */
    std::vector<CompositionOffsetEntry> compositionOffsets;
    /** The entries of the {@code stsc} box. */
    std::vector<SampleToChunkEntry> sampleToChunk;
    /** The offsets of the {@code stco} or {@code co64} box, in bytes from the start of the input. */
    std::vector<uint64_t> chunkOffsets;
    /** The sizes of the {@code stsz} or {@code stz2} box, empty when the size is constant. */
    std::vector<uint32_t> sampleSizes;
    /** The size of every sample, when the {@code stsz} box declares a constant one. */
    uint32_t constantSampleSize = 0;
    /** The one based numbers of the sync samples of the {@code stss} box. */
    std::vector<uint32_t> syncSamples;
    /** Whether not every sample is a sync sample, i.e. whether an {@code stss} box was read. */
    bool hasSyncSamples = false;
    /** The entries of the {@code elst} box. */
    std::vector<EditListEntry> editList;
    /** The number of samples the sample tables describe. */
    uint64_t sampleCount = 0;
    /** Whether the sample tables are complete enough to be used. */
    bool sampleTableValid = false;

    /** The output that receives the samples of the track. */
    std::shared_ptr<AVTrackOutput> output;
    /** The format that was last published for the track. */
    std::shared_ptr<MediaFormat> publishedFormat;
    /** The one based index of the sample description that was last published. */
    uint32_t publishedDescriptionIndex = 0;
    /** The index of the next sample of the sample tables to deliver. */
    uint64_t nextSampleIndex = 0;
    /** The samples of the movie fragments that were not delivered yet. */
    std::deque<FragmentSample> fragments;
    /** The decode time of the next movie fragment of the track, in its timescale. */
    int64_t fragmentDecodeTime = 0;
    /** Whether the decode time of the next movie fragment is known. */
    bool fragmentDecodeTimeKnown = false;

    // The cursors that turn a sample index into a timestamp and into a position. They are only
    // valid while the samples are read in increasing order; seek positions them again.

    /** The index of the {@code stts} entry that contains the next sample. */
    size_t timeEntryIndex = 0;
    /** The number of samples of that entry that were already consumed. */
    uint64_t timeEntryUsed = 0;
    /** The decode time of the next sample, in the timescale of the track. */
    int64_t decodeTime = 0;
    /** The index of the {@code ctts} entry that contains the next sample. */
    size_t offsetEntryIndex = 0;
    /** The number of samples of that entry that were already consumed. */
    uint64_t offsetEntryUsed = 0;
    /** The composition offset of the next sample, in the timescale of the track. */
    int64_t compositionOffset = 0;
    /** The index of the {@code stsc} entry that contains the next sample. */
    size_t chunkEntryIndex = 0;
    /** The index of the chunk that contains the next sample. */
    uint64_t chunkIndex = 0;
    /** The index of the first sample of that chunk. */
    uint64_t chunkFirstSample = 0;
    /** The one based index of the sample description of that chunk. */
    uint32_t chunkDescriptionIndex = 1;
    /** Whether the chunk cursor was positioned at least once since the last reset. */
    bool chunkCursorValid = false;

    /** The absolute position of the current sample. */
    uint64_t currentSampleOffset = 0;
    /** The size of the current sample. */
    uint32_t currentSampleSize = 0;
    /** The timestamp of the current sample, in microseconds. */
    int64_t currentSampleTimeUs = Definitions::kTimeUnset;
    /** The flags of the current sample. */
    uint32_t currentSampleFlags = static_cast<uint32_t>(AVSample::kFlagNone);
    /** The one based index of the sample description of the current sample. */
    uint32_t currentDescriptionIndex = 1;
    /** Whether the current sample comes from a movie fragment. */
    bool currentFromFragment = false;
    /** Whether the current sample fields describe a sample that can be delivered. */
    bool currentSampleValid = false;
  };

  // ================================================================================================
  // Sniffing
  // ================================================================================================

  /**
   * Returns whether the given four character code starts an ISO base media box that the extractor
   * understands. Mirrors the atom type switch of {@code Sniffer.sniffInternal}.
   */
  static bool isMp4Box(uint32_t fourCc);

  // ================================================================================================
  // Streaming box reading
  // ================================================================================================

  /**
   * Reads the header of the next box and decides how its payload is handled. Mirrors the
   * {@code STATE_READING_ATOM_HEADER} case of {@code Mp4Extractor.read}, including the {@code 1}
   * largesize and {@code 0} end of input size rules.
   *
   * @return {@code false} when the end of the input was reached.
   */
  bool readAtomHeader(AVExtractorInput& input);

  /**
   * Skips a bounded number of bytes of the payload of the current box, so that a huge {@code mdat}
   * is never buffered. Mirrors the payload skipping of the
   * {@code STATE_READING_ATOM_PAYLOAD} case of {@code Mp4Extractor.read}.
   *
   * @return {@code false} when the end of the input was reached.
   */
  bool skipAtomPayload(AVExtractorInput& input);

  /**
   * Buffers and parses the payload of the current box, which is a {@code moov} or a {@code moof}
   * box. Mirrors {@code Mp4Extractor.processMoovAtom} and {@code Mp4Extractor.processMoofAtom}.
   *
   * @return {@code false} when the end of the input was reached.
   */
  bool readAtomPayload(AVExtractorInput& input);

  /**
   * Chooses the state that follows the completion of a box: the samples are delivered when a track
   * has pending samples, otherwise the next box header is read.
   */
  void onBoxFinished();

  // ================================================================================================
  // Buffered box parsing
  // ================================================================================================

  /**
   * Reads the header of the box that starts at the given offset of the given buffer, clamped to the
   * end of the enclosing box.
   *
   * @param data The buffer that holds the box.
   * @param offset The offset of the box in the buffer.
   * @param end The offset of the end of the enclosing box, i.e. the largest offset the box may
   *            reach.
   *
   * @return The header, with {@link BoxHeader#valid} cleared when the box is malformed.
   */
  static BoxHeader readBoxHeader(const AVDataBuffer& data, uint64_t offset, uint64_t end);

  /**
   * Parses a {@code moov} box, creates the tracks it declares and declares the seek map. Mirrors
   * {@code Mp4MoovAtom} and {@code Mp4Extractor.processMoovAtom}.
   */
  void parseMoov(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a {@code trak} box. Mirrors {@code Mp4TrakAtom}.
   */
  void parseTrak(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a {@code tkhd} box, i.e. the identifier, the duration and the rotation of a track.
   * Mirrors {@code AtomParsers.parseTkhd}.
   */
  static void parseTkhd(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses an {@code edts} box and the {@code elst} box it contains. Mirrors
   * {@code AtomParsers.parseEdts}.
   */
  static void parseEdts(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code mdia} box. Mirrors {@code Mp4MdiaAtom}.
   */
  static void parseMdia(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code mdhd} box, i.e. the timescale, the duration and the language of a track.
   * Mirrors {@code AtomParsers.parseMdhd}.
   */
  static void parseMdhd(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code hdlr} box, i.e. the type of the media of a track. Mirrors
   * {@code AtomParsers.parseHdlr}.
   */
  static void parseHdlr(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code minf} box, which contains the sample tables. Mirrors {@code Mp4MinfAtom}.
   */
  static void parseMinf(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code stbl} box. Mirrors {@code Mp4StblAtom}.
   */
  static void parseStbl(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses an {@code stsd} box and the sample entries it contains. Mirrors {@code Mp4StsdAtom} and
   * {@code AtomParsers.parseTrack}.
   */
  static void parseStsd(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses one sample entry of an {@code stsd} box, i.e. an audio or a video sample description
   * with the configuration boxes it contains. Mirrors {@code AtomParsers.parseAudioSampleEntry} and
   * {@code AtomParsers.parseVideoSampleEntry}.
   */
  static void parseSampleEntry(AVDataBuffer& data, uint64_t offset, uint64_t size,
                               SampleDescription& description);

  /**
   * Parses the children of an audio sample entry, i.e. the {@code esds}, {@code dOps},
   * {@code dfLa}, {@code alac}, {@code wave}, {@code sgpd}, {@code sbgp} and {@code btrt} boxes.
   * Mirrors the child atom loop of {@code AtomParsers.parseAudioSampleEntry}.
   *
   * @param version The version of the sample entry, which tells whether it is a QuickTime one.
   */
  static void parseAudioSampleEntryChildren(AVDataBuffer& data, uint64_t offset, uint64_t size,
                                            uint32_t version, SampleDescription& description);

  /**
   * Parses the children of a video sample entry, i.e. the {@code avcC}, {@code hvcC},
   * {@code av1C}, {@code esds} and {@code btrt} boxes. Mirrors the child atom loop of
   * {@code AtomParsers.parseVideoSampleEntry}.
   */
  static void parseVideoSampleEntryChildren(AVDataBuffer& data, uint64_t offset, uint64_t size,
                                            SampleDescription& description);

  /**
   * Parses an {@code esds} box and the MPEG-4 elementary stream descriptors it contains. Mirrors
   * {@code AtomParsers.parseEsds}.
   */
  static void parseEsds(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /**
   * Parses an {@code avcC} box and produces the SPS and PPS codec specific data. Mirrors
   * {@code AtomParsers.parseAvcC}.
   */
  static void parseAvcC(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /**
   * Parses an {@code hvcC} box. Mirrors {@code AtomParsers.parseHvcC}.
   */
  static void parseHvcC(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /**
   * Parses an {@code av1C} box. Mirrors {@code AtomParsers.parseAv1C}.
   */
  static void parseAv1C(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /** Parses a `colr` box and fills the color space, range and transfer of the description. */
  static void parseColr(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /** Parses an `mdcv` box and fills the mastering display part of the HDR static info. */
  static void parseMdcv(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /** Parses a `clli` box and fills the content light level part of the HDR static info. */
  static void parseClli(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /** Parses a Dolby Vision `dvcC`/`dvvC` configuration record. */
  static void parseDolbyVisionConfig(AVDataBuffer& data, uint64_t offset, uint64_t size,
                                     SampleDescription& description);

  /** Parses the `d263` profile and level box of an H.263 sample entry. */
  static void parseD263(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        SampleDescription& description);

  /**
   * Reads the audio object type, the sample rate and the channel count of an AudioSpecificConfig.
   * Mirrors the sampling frequency table and the decoding of the AAC configuration.
   *
   * @return {@code false} when the configuration is too short to be read.
   */
  static bool parseAudioSpecificConfig(const std::vector<uint8_t>& config, int& sampleRate,
                                       int& channelCount, int& audioObjectType);

  /**
   * Parses the {@code roll} recovery sample group of a QuickTime audio sample entry, i.e. its
   * {@code sbgp} and {@code sgpd} boxes, and stores the encoder delay or padding it declares.
   * Mirrors the roll recovery handling of {@code AtomParsers}.
   */
  static void parseRollRecoveryGroup(AVDataBuffer& data, uint64_t offset, uint64_t size,
                                     SampleDescription& description);

  /**
   * Parses an {@code stts} box. Mirrors {@code Mp4SttsAtom}.
   */
  static void parseStts(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code ctts} box. Mirrors {@code Mp4CttsAtom}.
   */
  static void parseCtts(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses an {@code stsc} box. Mirrors {@code Mp4StscAtom}.
   */
  static void parseStsc(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses an {@code stsz} or an {@code stz2} box. Mirrors {@code Mp4StszAtom}.
   */
  static void parseStsz(AVDataBuffer& data, uint64_t offset, uint64_t size, bool compact,
                        Track& track);

  /**
   * Parses an {@code stco} or a {@code co64} box. Mirrors {@code Mp4StcoAtom}.
   *
   * @param is64Bit Whether the box stores 64 bit chunk offsets.
   */
  static void parseStco(AVDataBuffer& data, uint64_t offset, uint64_t size, bool is64Bit,
                        Track& track);

  /**
   * Parses an {@code stss} box. Mirrors {@code Mp4StssAtom}.
   */
  static void parseStss(AVDataBuffer& data, uint64_t offset, uint64_t size, Track& track);

  /**
   * Parses a {@code mvex} box and the {@code trex} boxes it contains. Mirrors {@code Mp4MvexAtom}.
   */
  void parseMvex(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a {@code udta} box, i.e. the file-level metadata of the movie: the ISO 6709 location, the
   * {@code meta} box with its {@code keys} and {@code ilst} boxes, and the Nero chapter list.
   * Mirrors {@code BoxParser.parseUdta}.
   */
  void parseUdta(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses the {@code meta} box of {@code udta}, i.e. its {@code keys} and {@code ilst} boxes.
   * Mirrors the {@code meta} handling of {@code BoxParser.parseUdta}.
   */
  void parseMetaBox(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a {@code keys} box into the identifiers of the items of the {@code ilst} box.
   */
  static void parseKeys(AVDataBuffer& data, uint64_t offset, uint64_t size,
                        std::vector<std::string>& keys);

  /**
   * Parses an {@code ilst} box, whose items are keyed by index into the {@code keys} box.
   */
  void parseIlst(AVDataBuffer& data, uint64_t offset, uint64_t size,
                 const std::vector<std::string>& keys);

  /**
   * Parses a {@code chpl} box, i.e. the chapter list of the movie, whose times are in 100 ns units.
   */
  void parseChpl(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a top level {@code uuid} box, keeping the XMP data of the registered UUID.
   */
  void parseUuid(AVDataBuffer& data, uint64_t offset, uint64_t size);

  /**
   * Parses a top-level {@code emsg} box and appends its payload as a sample of the emsg metadata
   * track. The playback timestamp comes from the event message's own timescale and presentation
   * time, not from the movie timeline.
   */
  void parseEmsg(AVDataBuffer& data, uint64_t offset, uint64_t size, uint64_t payloadPosition);

  /**
   * Publishes the file-level metadata to the tracks whose format was already declared.
   *
   * <p>The tracks are declared when the movie box was parsed, which happens before a top level
   * {@code uuid} box that follows it: the format of such a track is published again with the
   * metadata, so that a caller that holds the format observes it.
   */
  void applyMovieMetadata();

  /**
   * Parses a {@code moof} box and appends the samples of its track fragments to the tracks they
   * belong to. Mirrors {@code Mp4Extractor.processMoofAtom}.
   *
   * @param moofStartPosition The absolute position of the {@code moof} box in the input, which is
   *                          the default base data offset of its track fragments.
   */
  void parseMoof(AVDataBuffer& data, uint64_t offset, uint64_t size, uint64_t moofStartPosition);

  /**
   * Parses a {@code traf} box, i.e. one track fragment, with its {@code tfhd}, {@code tfdt} and
   * {@code trun} boxes. Mirrors {@code Mp4TrafAtom}.
   */
  void parseTraf(AVDataBuffer& data, uint64_t offset, uint64_t size, uint64_t moofStartPosition);

  // ================================================================================================
  // Track and format management
  // ================================================================================================

  /**
   * Returns the track with the given container identifier, or null when the extractor does not
   * expose it.
   */
  [[nodiscard]] Track* findTrack(uint32_t trackId);

  /**
   * Creates the output of every supported track and declares the seek map, exactly once. Mirrors
   * the track creation loop of {@code Mp4Extractor.processMoovAtom}.
   */
  void createTracksAndSeekMap();

  /**
   * Declares the seek map of the container. The seek points come from the sync samples of the
   * sample table of the most significant track, or from its chunk starts when the track has no
   * {@code stss} box, which is what the {@code IndexSeekMap} of the Java player is built from.
   */
  void emitSeekMap();

  /**
   * Visits the seek point candidates of the given track, i.e. its sync samples, or the first sample
   * of every chunk when the track has no {@code stss} box. It is called twice so that the number of
   * seek points stays bounded whatever the sample table declares.
   *
   * @param collect When false the points are only counted.
   * @param stride The period of the candidates that are kept, at least one.
   * @param points The vector the kept points are appended to when {@code collect} is true.
   *
   * @return The number of candidates the track has.
   */
  static uint64_t forEachSeekPoint(const Track& track, bool collect, uint64_t stride,
                                   std::vector<AVSeekMap::SeekPoint>& points);

  /**
   * Returns the track whose sample table describes the seek map, preferring video over audio, or
   * null when no track has a usable sample table.
   */
  [[nodiscard]] const Track* findSeekTrack() const;

  /**
   * Builds the format of the given sample description. Mirrors the {@code MediaFormat} that
   * {@code AtomParsers.parseTrack} produces.
   */
  [[nodiscard]] std::shared_ptr<MediaFormat> buildFormat(const Track& track,
                                                         const SampleDescription& description) const;

  /**
   * Publishes the format of the given sample description when it differs from the last published
   * one. Mirrors {@code Mp4Track.output.format(...)}.
   */
  void publishFormat(Track& track, uint32_t sampleDescriptionIndex);

  /** Delivers one cc_data sample to the caption track derived from @p track. */
  void emitCaptionSample(Track& track, const std::vector<uint8_t>& ccData, int64_t timeUs);

  /**
   * Returns the number of bytes of the length prefix of the NAL units of the given sample description,
   * or 0 when the description does not declare length prefixed NAL units.
   *
   * <p>Mirrors the framing that the AVC and HEVC configuration records declare: their fifth and 22nd
   * byte hold the size of the prefix, minus one.
   */
  [[nodiscard]] int nalLengthSize(const Track& track, uint32_t sampleDescriptionIndex) const;

  /**
   * Returns the given sample in the Annex-B framing the platform decoders read, or the sample itself
   * when it is not length prefixed.
   *
   * <p>The decoders of the platforms are given NAL units that are separated by start codes, which is
   * the framing the platform extractor converts its length prefixed samples to with the
   * {@code h264_mp4toannexb} and {@code hevc_mp4toannexb} bitstream filters. A container that stores
   * its samples length prefixed is converted here, so that the framing the decoders are given does not
   * depend on the extractor that read the container. The parameter sets of the entry are prepended to
   * a key frame, which is what the bitstream filters do as well.
   */
  [[nodiscard]] std::shared_ptr<AVBlockBuffer> toAnnexBIfNeeded(std::shared_ptr<AVBlockBuffer> sample,
                                                                const Track& track,
                                                                uint32_t sampleDescriptionIndex,
                                                                bool keyFrame) const;

  /**
   * Returns the parameter sets of the given AVC sample description, framed for an Annex-B sample, or an
   * empty vector when the description declares none.
   */
  [[nodiscard]] std::vector<uint8_t> collectParameterSets(const Track& track,
                                                          uint32_t sampleDescriptionIndex) const;

  /**
   * Validates the sample tables of a track once every box of its {@code stbl} box was parsed, and
   * computes its number of samples, its largest sample size and its duration. Mirrors the
   * consistency checks of the {@code Mp4SampleTable} constructor.
   */
  void finalizeTrack(Track& track);

  // ================================================================================================
  // Sample reading
  // ================================================================================================

  /**
   * Delivers the sample that comes first in the input, skips to it when it is ahead of the current
   * position, and returns {@code false} when no track has a pending sample. Mirrors
   * {@code Mp4Extractor.readSample}.
   */
  bool readSample(AVExtractorInput& input);

  /**
   * Positions the sample cursors of the track on the given sample index. Mirrors the random access
   * of {@code Mp4SampleTable}.
   */
  static void setSampleIndex(Track& track, uint64_t sampleIndex);

  /**
   * Updates the fields that describe the current sample of the track: its offset, its size, its
   * timestamp, its flags and its sample description.
   */
  static void updateCurrentSample(Track& track);

  /**
   * Moves the cursor of the track to its next sample. Mirrors the iteration of
   * {@code Mp4SampleTable}.
   */
  static void advanceSample(Track& track);

  /**
   * Returns the size of the given sample of the track, in bytes. Mirrors
   * {@code Mp4SampleTable.getSampleSize}.
   */
  [[nodiscard]] static uint32_t sampleSize(const Track& track, uint64_t sampleIndex);

  /**
   * Returns whether the given sample of the track is a sync sample. Mirrors
   * {@code Mp4SampleTable.getSampleSync}, where a track without an {@code stss} box has every
   * sample flagged as a sync sample.
   */
  [[nodiscard]] static bool isSyncSample(const Track& track, uint64_t sampleIndex);

  /**
   * Positions the chunk cursor of the track on the chunk that contains the given sample. Mirrors
   * the sample to chunk resolution of {@code Mp4SampleTable}.
   *
   * @return Whether the sample could be resolved.
   */
  static bool positionChunkCursor(Track& track, uint64_t sampleIndex);

  /**
   * Returns the index of the first sample of the track whose timestamp is not smaller than the
   * requested time, snapped back to the sync sample that precedes it. Mirrors the sample search of
   * {@code Mp4Extractor.seek} and of {@code Mp4SampleTable}.
   */
  [[nodiscard]] static uint64_t findSampleIndexForTime(const Track& track, int64_t timeUs);

  /**
   * Returns the index of the first sample of the track that ends after the given position.
   */
  [[nodiscard]] static uint64_t findSampleIndexForPosition(const Track& track, long position);

  /**
   * Returns the media time of the first non empty edit of the track, in its timescale, i.e. the
   * time of the first sample that its edit list presents. Mirrors the edit list handling of
   * {@code AtomParsers.parseTrack}.
   */
  [[nodiscard]] static int64_t editStartTicks(const Track& track);

  /**
   * Returns whether any track has a sample to deliver.
   */
  [[nodiscard]] bool hasPendingSamples();

  /**
   * Reports the end of the input: the tracks are ended, the seek map is declared when the container
   * never declared one, and the position is refreshed. Mirrors the end of the read loop of
   * {@code Mp4Extractor}.
   */
  Result finish(AVExtractorInput& input);

  // ================================================================================================
  // State
  // ================================================================================================

  /**
   * The output that receives the tracks, the formats and the samples. It is owned by the reader,
   * which outlives the extractor, therefore the extractor only observes it.
   */
  AVExtractorOutput* output_ = nullptr;
  /** The state of the box parser. */
  State state_ = State::kReadingAtomHeader;
  /** The buffer that holds the payload of the {@code moov} and {@code moof} boxes. */
  AVDataBuffer payload_;
  /** The four character code of the box that is being read. */
  uint32_t atomType_ = 0;
  /** The number of payload bytes of the current box that were not consumed yet. */
  uint64_t atomPayloadRemaining_ = 0;
  /** The absolute position of the current box in the input. */
  uint64_t atomStartPosition_ = 0;
  /** The absolute position of the current box payload in the input. */
  uint64_t atomPayloadStartPosition_ = 0;
  /** The length of the input, or -1 when it is unknown. */
  long inputLength_ = -1;
  /** Whether the end of the input was reached. */
  bool endOfInput_ = false;
  /** The tracks of the container, ordered by the identifier they were given. */
  std::vector<Track> tracks_;
  /** The index of the synthetic top-level emsg metadata track, or -1 when none was created. */
  int emsgTrackIndex_ = -1;
  /** The output of the emsg metadata track, when the movie is fragmented. */
  std::shared_ptr<AVTrackOutput> emsgOutput_;
  /** Whether the movie box declared movie fragments. */
  bool fragmented_ = false;
  /** The defaults that the {@code trex} boxes declare, indexed by track identifier. */
  std::map<uint32_t, TrackExtendsDefaults> trackExtendsDefaults_;
  /** The timescale of the movie, as declared by the {@code mvhd} box. */
  int64_t movieTimescale_ = 0;
  /** The duration of the movie in microseconds, or {@link Definitions::kTimeUnset}. */
  int64_t movieDurationUs_ = Definitions::kTimeUnset;
  /**
   * The file-level metadata that the {@code udta} and the XMP {@code uuid} boxes declared. It is
   * attached to every track format, as the Java extractor does.
   */
  std::vector<std::shared_ptr<Metadata::Entry>> movieMetadata_;
  /** Whether the seek map was declared, so that it is declared exactly once. */
  bool seekMapEmitted_ = false;
  /**
   * Whether the movie box was parsed.
   *
   * <p>The movie box describes the tracks and the samples of a non-fragmented movie, so it is parsed
   * exactly once: a movie whose box follows the media data it describes is reached a second time once
   * the samples of the box were delivered, and parsing it again would add the same tracks, with the
   * sample cursors of a movie that was already read.
   */
  bool movieBoxParsed_ = false;
  /** Whether the end of the tracks was signalled. */
  bool tracksEnded_ = false;
  /** The position last observed on the input. */
  long position_ = 0;
  /**
   * The position of the input that follows the last top level box that was read, i.e. where the scan
   * of the top level boxes resumes once the samples of the movie box were delivered. Trailers such
   * as an XMP {@code uuid} box may follow the movie box of a non-fragmented file.
   */
  uint64_t topLevelResumePosition_ = 0;
  /** The reason why {@link #sniff} rejected the input. */
  std::string sniffFailureReason_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_CONTAINER_MP4_EXTRACTOR_H
