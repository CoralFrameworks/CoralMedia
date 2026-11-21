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

#ifndef CORAL_EXTRACTOR_TEXT_WEBVTT_MP4_WEBVTT_PARSER_H
#define CORAL_EXTRACTOR_TEXT_WEBVTT_MP4_WEBVTT_PARSER_H

#include <extractor/text/SubtitleParser.h>

#include <cstddef>
#include <cstdint>

namespace coral {

/**
 * The parser of the WebVTT samples of an MP4 container.
 *
 * <p>This is the counterpart of ExoPlayer's {@code Mp4WebvttParser}: a sample is a list of boxes,
 * each `vttc` box holding the settings (`sttg`) and the text (`payl`) of one cue, and the timing of
 * every cue is the one of the sample rather than a timestamp inside the data. That is why the cues
 * of a sample are emitted as one result whose {@link AVCuesWithTiming::startTimeUs} is
 * {@link Definitions::kTimeUnset}.
 *
 * <p>A sample replaces the cues of the previous one, so the replacement behavior of the track is
 * {@link CueReplacementBehavior::kReplace}.
 */
class AVMp4WebvttParser final : public AVSubtitleParser {
public:
  /** The cue replacement behavior of the format, see {@link AVSubtitleParserFactory}. */
  static constexpr CueReplacementBehavior kCueReplacementBehavior =
    CueReplacementBehavior::kReplace;

  AVMp4WebvttParser() = default;

  ~AVMp4WebvttParser() override = default;

  bool parse(const uint8_t* data, size_t size, const OutputOptions& options,
             const CueConsumer& output) override;

  void reset() override;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_WEBVTT_MP4_WEBVTT_PARSER_H
