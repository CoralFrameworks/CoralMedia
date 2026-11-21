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

#ifndef CORAL_EXTRACTOR_TEXT_SUBTITLE_PARSER_H
#define CORAL_EXTRACTOR_TEXT_SUBTITLE_PARSER_H

#include <extractor/text/CuesWithTiming.h>

#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace coral {

/**
 * Parses the samples of a subtitle track into timed cues.
 *
 * <p>This is the counterpart of ExoPlayer's {@code androidx.media3.extractor.text.SubtitleParser}.
 * An instance is stateful, because a subtitle format may spread one cue over several samples and a
 * text file may only be complete at its end: samples are fed in the order the container delivers
 * them, and every complete result is handed to the consumer of the call. A caller that seeks calls
 * {@link reset()} before it feeds the samples of the new position.
 *
 * <p>A parser never throws and never allocates according to an unchecked length of the sample: a
 * malformed subtitle sample is reported as a {@code false} return value, which the caller logs and
 * skips rather than turning into a playback failure.
 */
class AVSubtitleParser {
public:
  /**
   * What a parse call emits, and in which order.
   *
   * <p>Mirrors ExoPlayer's {@code SubtitleParser.OutputOptions}: a text file can be parsed at a seek
   * position without first emitting every cue before it, which is what makes seeking into a subtitle
   * file with thousands of cues cheap.
   */
  struct OutputOptions final {
    /**
     * The cues at or after this time are emitted first; {@link Definitions::kTimeUnset} emits every
     * cue in the order of the data.
     */
    int64_t startTimeUs = Definitions::kTimeUnset;

    /**
     * Whether the cues before {@link #startTimeUs} are emitted after the ones after it. Ignored when
     * {@link #startTimeUs} is unset.
     */
    bool outputAllCues = true;

    /** Emits every cue, in the order of the data. */
    static OutputOptions allCues();

    /** Emits the cues that start at or after @p startTimeUs only. */
    static OutputOptions onlyCuesAfter(int64_t startTimeUs);

    /** Emits the cues after @p startTimeUs first, then the remaining ones. */
    static OutputOptions cuesAfterThenRemainingCuesBefore(int64_t startTimeUs);
  };

  /** The consumer of the results of one {@link #parse} call. */
  using CueConsumer = std::function<void(const AVCuesWithTiming&)>;

  virtual ~AVSubtitleParser() = default;

  /**
   * Feeds subtitle data to the parser.
   *
   * <p>The data of a call must be a whole sample, or the whole contents of a subtitle file: a format
   * that needs more data keeps what it cannot parse yet and continues on the next call. The consumer
   * is called on the calling thread, before the call returns.
   *
   * @param data    The bytes to parse.
   * @param size    The number of bytes to read from @p data.
   * @param options The order in which the cues are emitted.
   * @param output  The consumer of the complete results.
   *
   * @return {@code false} when the data is malformed, in which case the caller skips the sample.
   */
  virtual bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
                     const CueConsumer& output) = 0;

  /** Drops the state that the parsers of the samples before a seek accumulated. */
  virtual void reset() = 0;
};

/**
 * Creates the parser of a subtitle format, and describes the format.
 *
 * <p>This is the counterpart of ExoPlayer's {@code SubtitleParser.Factory}. A factory answers for a
 * {@link MediaFormat} rather than for a string of bytes, because the container already told which
 * format the samples are in.
 */
class AVSubtitleParserFactory {
public:
  virtual ~AVSubtitleParserFactory() = default;

  /**
   * Returns whether this factory can parse the samples of the given format.
   *
   * <p>A format that carries no subtitle MIME type is not supported: a factory does not guess.
   */
  [[nodiscard]] virtual bool supportsFormat(const MediaFormat& format) const = 0;

  /**
   * Returns the replacement behavior of the cues of the given format.
   *
   * <p>The parser of a format that is not supported must still answer this, because a player reads
   * the behavior to decide whether the cues of a sample replace the ones that are shown even when it
   * has no parser and drops the track.
   */
  [[nodiscard]] virtual CueReplacementBehavior
  getCueReplacementBehavior(const MediaFormat& format) const = 0;

  /**
   * Creates a parser for @p format.
   *
   * @return The parser, or null when {@link supportsFormat} is false.
   */
  [[nodiscard]] virtual std::unique_ptr<AVSubtitleParser>
  create(const MediaFormat& format) const = 0;
};

/**
 * Returns the replacement behavior of the cues of a subtitle MIME type.
 *
 * <p>The values follow ExoPlayer: the formats that carry one cue set per sample and durations (the
 * WebVTT and SubRip styles) merge their cues into the ones that are still shown, whereas a format
 * that carries a complete cue set per sample (MP4 VR-style cues) replaces them. The default for an
 * unknown type is {@link CueReplacementBehavior::kMerge}, which is also ExoPlayer's default.
 */
[[nodiscard]] CueReplacementBehavior cueReplacementBehaviorForMimeType(const std::string& mimeType);

/**
 * Creates the factory of the subtitle parsers that the extractor module knows.
 *
 * <p>The factory is the one the compose player and the playback player use, so that both support the
 * same formats. It never returns null; a module build that registers no parser yields a factory that
 * supports nothing rather than a null pointer a caller has to test.
 */
[[nodiscard]] std::unique_ptr<AVSubtitleParserFactory> createDefaultSubtitleParserFactory();

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_SUBTITLE_PARSER_H
