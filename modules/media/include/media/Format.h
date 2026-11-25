/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_MEDIA_FORMAT_H
#define CORAL_MEDIA_FORMAT_H

#include <media/ColorInfo.h>
#include <media/Definitions.h>
#include <media/Metadata.h>
#include <media/Types.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Encapsulates the information describing the format of media data, be it audio or video,
 * as well as optional feature metadata.
 */
class MediaFormat final {
public:
  /**
   * A value for various fields to indicate that the field's value is unknown or not applicable.
   */
  static constexpr int NO_VALUE = Definitions::kUnspecified;

  /**
   * A value for {@link #getSubsampleOffsetUs()} to indicate that the offset is relative to each
   * sample, rather than to the start of the stream. Mirrors ExoPlayer's
   * {@code Format.OFFSET_SAMPLE_RELATIVE}.
   */
  static constexpr int64_t OFFSET_SAMPLE_RELATIVE = std::numeric_limits<int64_t>::min();

  static std::shared_ptr<MediaFormat>
  CreateAudioFormat(const std::string& mime, int sampleRate, int channelCount);

  static std::shared_ptr<MediaFormat>
  CreateVideoFormat(const std::string& mime, int width, int height);

  /**
   * Creates the format of a subtitle track.
   *
   * <p>A subtitle is neither audio nor video, so neither of the two factories above describes it: a
   * format that was created as audio would report a sample rate of zero and a format that was
   * created as video would report a size of zero, and both would be wrong rather than unset.
   */
  static std::shared_ptr<MediaFormat> CreateSubtitleFormat(const std::string& mime);

  void setId(const int newId) { id = newId; }

  [[nodiscard]] int getId() const { return id; }

  void setLabel(const std::string& newLabel) { label = newLabel; }

  [[nodiscard]] const std::string& getLabel() const { return label; }

  [[nodiscard]] int getAverageBitrate() const { return mAverageBitrate; }

  void setAverageBitrate(int bitrate);

  [[nodiscard]] int getPeakBitrate() const { return mPeakBitrate; }

  void setPeakBitrate(int bitrate);

  [[nodiscard]] const std::string& getContainerMimeType() const { return mContainerMimeType; }

  void setContainerMimeType(const std::string& mimeType) {
    mContainerMimeType = mimeType;
  }

  [[nodiscard]] const std::string& getMimeType() const { return mMimeType; }

  void setMimeType(const std::string& mimeType) { mMimeType = mimeType; }

  [[nodiscard]] const std::string& getLanguage() const { return mLanguage; }

  void setLanguage(const std::string& language) { mLanguage = language; }

  [[nodiscard]] MediaSelectionFlags getSelectionFlags() const { return mSelectionFlags; }

  void setSelectionFlags(const MediaSelectionFlags flags) {
    mSelectionFlags = flags;
  }

  /**
   * Returns the role flags of the track, i.e. the {@link RoleFlags} bit mask that describes what
   * the track is for, e.g. whether it is a caption track or an audio description.
   */
  [[nodiscard]] uint32_t getRoleFlags() const { return mRoleFlags; }

  void setRoleFlags(const uint32_t roleFlags) { mRoleFlags = roleFlags; }

  /**
   * Returns the CEA-708 caption service number this track carries, or 0 when the track is not a
   * CEA-708 track or the service is unknown.
   */
  [[nodiscard]] int getCaptionServiceNumber() const { return mCaptionServiceNumber; }

  /** Sets the CEA-708 caption service number of the track. */
  void setCaptionServiceNumber(const int serviceNumber) { mCaptionServiceNumber = serviceNumber; }

  [[nodiscard]] int64_t getDurationUs() const { return mDurationUs; }

  void setDurationUs(const int64_t durationUs) { mDurationUs = durationUs; }

  [[nodiscard]] int getTrackId() const { return mTrackId; }

  void setTrackId(const int trackId) { mTrackId = trackId; }

  /**
   * Returns whether the track was derived from another track instead of being declared by the
   * container, e.g. the CEA-708 caption track that an extractor derives from the SEI of an
   * H.264/H.265 video track. The samples of a derived track are already carried by its source
   * track, so a muxer rejects it and a remux skips it.
   */
  [[nodiscard]] bool isDerivedTrack() const { return mDerivedTrack; }

  /** Marks the track as derived from another track, see {@link #isDerivedTrack()}. */
  void setDerivedTrack(const bool derived) { mDerivedTrack = derived; }

  [[nodiscard]] int getFrameCount() const { return mFrameCount; }

  void setFrameCount(const int frameCount) { mFrameCount = frameCount; }

  [[nodiscard]] int getSampleRate() const { return mSampleRate; }

  void setSampleRate(const int sampleRate) { mSampleRate = sampleRate; }

  [[nodiscard]] int getChannelCount() const { return mChannelCount; }

  void setChannelCount(const int channelCount) { mChannelCount = channelCount; }

  [[nodiscard]] const AudioFormatType& getAudioFormatType() const { return mAudioFormatType; }

  void setAudioFormatType(const AudioFormatType codecType) {
    mAudioFormatType = codecType;
  }

  [[nodiscard]] const AudioSampleFormat& getSampleFormat() const { return mSampleFormat; }

  void setSampleFormat(const AudioSampleFormat format) {
    mSampleFormat = format;
  }

  [[nodiscard]] int getEncoderDelay() const { return mEncoderDelay; }

  void setEncoderDelay(const int encoderDelay) { mEncoderDelay = encoderDelay; }

  [[nodiscard]] int getEncoderPadding() const { return mEncoderPadding; }

  void setEncoderPadding(const int encoderPadding) {
    mEncoderPadding = encoderPadding;
  }

  [[nodiscard]] int getWidth() const { return mWidth; }

  void setWidth(const int width) { mWidth = width; }

  [[nodiscard]] int getHeight() const { return mHeight; }

  void setHeight(const int height) { mHeight = height; }

  [[nodiscard]] VideoCodecType getVideoCodecType() const { return mVideoCodecType; }

  void setVideoCodecType(const VideoCodecType videoCodecType) {
    mVideoCodecType = videoCodecType;
  }

  void setPixelFormat(const PixelFormat format) { mPixelFormat = format; }

  [[nodiscard]] PixelFormat getPixelFormat() const { return mPixelFormat; }

  /**
   * Requests the raw frame layout this codec should process.
   *
   * <p>The direction follows the codec's role: for a decoder it is the layout of the frames the
   * decoder produces, and for an encoder it is the layout of the frames the caller feeds it. This
   * is a hint rather than a promise: a codec that cannot honour it keeps its platform default, and
   * the caller falls back to a conversion. The default is {@link PixelFormat::kFormatInvalid},
   * which leaves the codec's choice untouched.
   *
   * <p>An Apple consumer that samples one BGRA IOSurface sets this to
   * {@link PixelFormat::kFormatBGRA}; VideoToolbox then converts YUV to RGB while decoding, and the
   * frame needs no second GPU conversion. No encoder honours the field yet; it is part of the
   * format so an encoder can later request its input layout without another API change.
   */
  void setProcessFormat(const PixelFormat format) { mProcessFormat = format; }

  [[nodiscard]] PixelFormat getProcessFormat() const { return mProcessFormat; }

  [[nodiscard]] int getFrameRate() const { return mFrameRate; }

  void setFrameRate(const int frameRate) { mFrameRate = frameRate; }

  [[nodiscard]] int getIFrameInterval() const { return mIFrameInterval; }

  void setIFrameInterval(int iFrameInterval) {
    mIFrameInterval = iFrameInterval;
  }

  [[nodiscard]] int getMaxBFrames() const { return mMaxBFrames; }

  void setMaxBFrames(const int maxBFrames) { mMaxBFrames = maxBFrames; }

  [[nodiscard]] int getMaxSubLayers() const { return mMaxSubLayers; }

  void setMaxSubLayers(const int maxSubLayers) { mMaxSubLayers = maxSubLayers; }

  [[nodiscard]] int getRotation() const { return mRotationDegrees; }

  void setRotation(const int rotation) { mRotationDegrees = rotation; }

  /** HDR/color information of the video, or default-constructed values when unknown. */
  [[nodiscard]] const ColorInfo& getColorInfo() const { return mColorInfo; }

  void setColorInfo(const ColorInfo& colorInfo) { mColorInfo = colorInfo; }

  [[nodiscard]] int getBitrate() const { return mBitrate; }

  [[nodiscard]] int getMaxInputSize() const { return mMaxInputSize; }

  void setMaxInputSize(const int maxInputSize) { mMaxInputSize = maxInputSize; }

  [[nodiscard]] int getProfile() const { return mProfile; }

  void setProfile(const int profile) { mProfile = profile; }

  [[nodiscard]] int getLevel() const { return mLevel; }

  void setLevel(const int level) { mLevel = level; }

  void setCodecSpecificData(int index, const uint8_t* data, size_t size);

  void setCodecSpecificData(int index, const std::vector<uint8_t>& data);

  void setCodecSpecificData(const std::vector<std::vector<uint8_t>>& data);

  [[nodiscard]] const std::shared_ptr<Metadata>& getMetadata() const {
    return mMetadata;
  }

  void setMetadata(const std::shared_ptr<Metadata>& metadata) {
    mMetadata = metadata;
  }

  [[nodiscard]] const std::string& getCodecs() const { return codecs; }

  void setCodecs(std::string newCodecs) { codecs = std::move(newCodecs); }

  [[nodiscard]] bool isAudio() const {
    return (mMimeType.find("audio/") != std::string::npos);
  }

  [[nodiscard]] bool isVideo() const {
    return (mMimeType.find("video/") != std::string::npos);
  }

  [[nodiscard]] bool isImage() const {
    return (mContainerMimeType.find("image/") != std::string::npos);
  }

  [[nodiscard]] bool isSubtitle() const {
    return MimeTypes::isText(mMimeType);
  }

  /**
   * Returns the replacement behavior of the cues of this text track.
   *
   * <p>Mirrors ExoPlayer's {@code Format.cueReplacementBehavior}: a player uses it to decide whether
   * the cues of a sample add to the ones that are still shown or replace them. The value is
   * meaningful for a subtitle format only, and defaults to {@link CueReplacementBehavior::kMerge}.
   */
  [[nodiscard]] CueReplacementBehavior getCueReplacementBehavior() const {
    return mCueReplacementBehavior;
  }

  void setCueReplacementBehavior(const CueReplacementBehavior behavior) {
    mCueReplacementBehavior = behavior;
  }

  /**
   * Returns the codec specific data list.
   */
  [[nodiscard]] const std::vector<std::vector<uint8_t>>& csdList() const {
    return mCsdList;
  }

  /**
   * Returns the number of pixels if this is a video format whose {@link mWidth}
   * and {@link mHeight} are known, or {@link NO_VALUE} otherwise.
   */
  [[nodiscard]] int getPixelCount() const {
    return (getWidth() == NO_VALUE || getHeight() == NO_VALUE) ? NO_VALUE : (getWidth() * getHeight());
  }

  /**
   * The offset in microseconds to add to the presentation timestamps of the samples of this format,
   * or {@link #OFFSET_SAMPLE_RELATIVE} when the offset is relative to each sample.
   */
  [[nodiscard]] int64_t getSubsampleOffsetUs() const { return mSubsampleOffsetUs; }

  void setSubsampleOffsetUs(const int64_t subsampleOffsetUs) {
    mSubsampleOffsetUs = subsampleOffsetUs;
  }

  [[nodiscard]] std::shared_ptr<MediaFormat> buildUpon() const;

  [[nodiscard]] std::string toString() const;

private:
  MediaFormat();

  void adjustBitRate();

  static constexpr int MAX_CSD_LENGTH = 3;

  /** An identifier for the format, or null if unknown or not applicable. */
  int id;

  /**
   * The default human-readable label, or null if unknown or not applicable.
   */
  std::string label;

  /**
   * An identifier for the format, or null if unknown or not applicable.
   */
  int mTrackId;

  /** Whether the track was derived from another track instead of declared by the container. */
  bool mDerivedTrack = false;

  /**
   * The duration in microseconds.
   */
  int64_t mDurationUs;

  /**
   * The frame count
   */
  int mFrameCount;

  /**
   * The language as an IETF BCP 47 conformant tag, or null if unknown or not
   * applicable.
   */
  std::string mLanguage;

  /**
   * Media selection flags.
   */
  MediaSelectionFlags mSelectionFlags;

  /**
   * Role flags, see {@link RoleFlags}.
   */
  uint32_t mRoleFlags = kRoleFlagNone;

  /** The CEA-708 caption service number, or 0 when unknown. */
  int mCaptionServiceNumber = 0;

  /**
   * The average bitrate in bits per second, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mAverageBitrate;

  /**
   * The peak bitrate in bits per second, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mPeakBitrate;

  /**
   * The bitrate in bits per second. This is the peak bitrate if known, or else the average bitrate if known,
   * or else {@link Definitions::kUnspecified}.
   * Equivalent to:
   * {@code peakBitrate != Definitions::kUnspecified ? peakBitrate : averageBitrate}.
   */
  int mBitrate;

  /**
   * Codecs of the format as described in RFC 6381, or null if unknown or not applicable.
   */
  std::string codecs;

  /** Metadata, or null if unknown or not applicable. */
  std::shared_ptr<Metadata> mMetadata;

  /**
   * The mime type of the container, or null if unknown or not applicable.
   *
   * For example, the Format is parsed from an audio file with .flac, the container mime type is "audio/flac".
   */
  std::string mContainerMimeType;

  /**
   * The sample mime type, or null if unknown or not applicable.
   */
  std::string mMimeType;

  /**
   * The maximum size of a buffer of data (typically one sample),
   * or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mMaxInputSize;

  /**
   * The profile for audio/video, or {@link Definitions::kUnspecified} if unknown or not applicable
   */
  int mProfile;

  /**
   * The level for audio/video, or {@link Definitions::kUnspecified} if unknown or not applicable
   */
  int mLevel;

  /**
   * Initialization data that must be provided to the decoder. Will not be null,
   * but may be empty if initialization data is not required.
   */
  std::vector<std::vector<uint8_t>> mCsdList;

  //--------------------------------- Audio specific -------------------------------------------
  /**
   * The number of audio channels, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mChannelCount;

  /**
   * The audio sampling rate in Hz, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mSampleRate;

  /**
   * The audio format type.
   */
  AudioFormatType mAudioFormatType;

  /**
   * The {@link AudioSampleFormat} for PCM audio.
   *
   * Set to {@link AudioSampleFormat::kInvalid} for other media types.
   */
  AudioSampleFormat mSampleFormat;

  /**
   * The number of frames to trim from the start of the decoded audio stream, or 0 if not applicable.
   */
  int mEncoderDelay;

  /**
   * The number of frames to trim from the end of the decoded audio stream, or 0 if not applicable.
   */
  int mEncoderPadding;

  //--------------------------------- Video specific. -------------------------------------------
  /**
   * The width of the video in pixels, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mWidth;

  /**
   * The height of the video in pixels, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mHeight;

  /**
   * The frame rate in frames per second, or {@link Definitions::kUnspecified} if unknown or not applicable.
   */
  int mFrameRate;

  /**
   * The clockwise rotation that should be applied to the video for it to be rendered in the correct orientation,
   * or 0 if unknown or not applicable.
   * Only 0, 90, 180 and 270 are supported.
   */
  int mRotationDegrees;

  /** The HDR/color information of the video. */
  ColorInfo mColorInfo;

  /**
   * The video codec type.
   */
  VideoCodecType mVideoCodecType;

  /**
   * The pixel format for the video frame, or {@link PixelFormat::kFormatInvalid} if unknown or not applicable
   */
  PixelFormat mPixelFormat;

  /**
   * The raw frame layout this codec should process, or {@link PixelFormat::kFormatInvalid} to keep
   * the codec's default. A decoder reads it as its output layout and an encoder as its input layout.
   */
  PixelFormat mProcessFormat;

  /**
   * The frame interval for the video, or {@link Definitions::kUnspecified} if unknown or not applicable
   */
  int mIFrameInterval;

  /**
   * The max number of the b-frame for video, or 0 if unknown or not applicable.
   */
  int mMaxBFrames;

  /**
   * The maximum number of temporal scalable sub-layers in the video bitstream, or {@link NO_VALUE} if not applicable.
   */
  int mMaxSubLayers;

  /**
   * The offset in microseconds to add to the presentation timestamps of the samples of this format,
   * or {@link OFFSET_SAMPLE_RELATIVE} when the offset is relative to each sample.
   */
  int64_t mSubsampleOffsetUs;

  /**
   * The replacement behavior of the cues of this text track, see
   * {@link CueReplacementBehavior}.
   */
  CueReplacementBehavior mCueReplacementBehavior;
};

} // namespace coral

#endif //CORAL_MEDIA_FORMAT_H
