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

#ifndef CORAL_EXTRACTOR_AUDIO_MP3_EXTRACTOR_H
#define CORAL_EXTRACTOR_AUDIO_MP3_EXTRACTOR_H

#include <extractor/DataBuffer.h>
#include <extractor/Extractor.h>
#include <extractor/Utility.h>

#include <media/Metadata.h>
#include <extractor/audio/MpegAudio.h>

#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Extracts the frames of an MP3 stream, i.e. of an MPEG-1/2/2.5 audio layer 3 stream.
 *
 * <p>The extractor skips the ID3 tags, resynchronizes on the next frame when the stream is damaged,
 * and understands the Xing/Info and VBRI headers so that the duration and the seek points of a
 * variable bitrate stream are known. When those headers are absent the stream is assumed to be of a
 * constant bitrate, which is what ExoPlayer's {@code Mp3Extractor} does.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code Mp3Extractor}.
 */
class AVMp3Extractor final : public AVExtractor {
public:
  AVMp3Extractor() = default;

  ~AVMp3Extractor() override = default;

  AVMp3Extractor(const AVMp3Extractor& other) = delete;

  AVMp3Extractor& operator=(const AVMp3Extractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "MP3"; }

  [[nodiscard]] std::string getSniffFailureReason() const override { return sniffFailureReason_; }

private:
  /**
   * A frame that was located in the stream.
   */
  struct Frame {
    /** The header of the frame. */
    AVMpegAudioHeader header;
    /** The size of the frame in bytes, header included. */
    int frameSize = 0;
    /** The position of the first byte of the frame. */
    long startPosition = -1;
  };

  /**
   * The description of a variable bitrate stream, as read from its Xing/Info or VBRI header.
   */
  struct VbrSeeker {
    /** Whether a Xing/Info or a VBRI header was found. */
    bool present = false;
    /** The number of frames of the stream, or -1 when unknown. */
    int frameCount = -1;
    /** The number of bytes of the stream, or -1 when unknown. */
    long byteCount = -1;
    /** The duration of the stream, in microseconds, or {@link Definitions::kTimeUnset}. */
    int64_t durationUs = Definitions::kTimeUnset;
    /** The average bitrate of the stream, in bits per second, or -1 when unknown. */
    int averageBitrate = -1;
  };

  /**
   * Reads the ID3v2 tags at the start of the stream, and leaves the read position at the first byte
   * of the audio data.
   *
   * @return {@code true} when the position of the first frame is known.
   */
  bool readId3Headers(AVExtractorInput& input);

  /**
   * Searches for the next frame header, and parses it.
   *
   * @param input The input to read from.
   * @param frame The frame that is filled in on success.
   *
   * @return {@code true} when a valid frame header was found.
   */
  bool findNextFrame(AVExtractorInput& input, Frame& frame);

  /** Reads an ID3 tag at the current position, if any, and emits it as a timed sample. */
  bool readMidStreamId3(AVExtractorInput& input, int64_t timeUs);

  /**
   * Reads the Xing/Info or the VBRI header of the first frame.
   *
   * @return {@code true} when such a header was found.
   */
  bool readVbrSeeker(const AVDataBuffer& frameData, const Frame& frame);

  /**
   * Reads a Xing or an Info header at the given offset of the frame.
   */
  bool readXingHeader(const AVDataBuffer& frameData, const Frame& frame, size_t offset);

  /**
   * Reads a VBRI header of the frame.
   */
  bool readVbriHeader(const AVDataBuffer& frameData, const Frame& frame);

  /**
   * Returns the presentation timestamp of the frame with the given index.
   */
  [[nodiscard]] int64_t frameTimeUs(int64_t frameIndex) const;

  /**
   * Returns the presentation timestamp of the frame at the given position, assuming a constant
   * bitrate.
   */
  [[nodiscard]] int64_t constantBitrateTimeUs(long framePosition) const;

  /**
   * Builds and publishes the seek map of the stream.
   */
  void buildSeekMap();

  /**
   * Builds the format of the track from the current frame header.
   */
  std::shared_ptr<MediaFormat> buildFormat() const;

  bool id3Read_ = false;
  bool formatPublished_ = false;
  /** The metadata that the ID3 tags of the stream declared, or null. */
  std::shared_ptr<Metadata> metadata_;
  /** The raw ID3v2 tag bytes, kept for the timed metadata track. */
  std::vector<uint8_t> id3Tags_;
  bool trackEnded_ = false;
  bool tracksEnded_ = false;
  /** Whether the stream is of a constant bitrate, and can therefore be seeked by interpolation. */
  bool constBitrate_ = true;
  AVMpegAudioHeader header_;
  long position_ = 0;
  int64_t frameIndex_ = 0;
  long streamStartPosition_ = 0;
  long firstFramePosition_ = -1;
  long dataEndPosition_ = -1;
  int64_t durationUs_ = Definitions::kTimeUnset;
  int averageBitrate_ = -1;
  VbrSeeker seeker_;
  std::vector<AVSeekMap::SeekPoint> seekPoints_;
  std::string sniffFailureReason_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  std::shared_ptr<AVTrackOutput> metadataOutput_;
  /**
   * The output, kept so that the seek map can be published once the first frame, and therefore the
   * VBR header of the stream, was read.
   */
  AVExtractorOutput* seekMapOutput_ = nullptr;
  AVDataBuffer scratch_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_AUDIO_MP3_EXTRACTOR_H
