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

#ifndef CORAL_CODEC_WMF_MEDIA_UTILS_H
#define CORAL_CODEC_WMF_MEDIA_UTILS_H

#include <codec/CodecTypes.h>
#include <codec/Error.h>

#include <media/Format.h>
#include <media/Types.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coral {

/**
 * The shared helpers for the Windows Media Foundation based implementations.
 */
class WmfMediaUtils final {
public:
  /**
   * Starts the Media Foundation platform. The call is reference counted, so every
   * successful call must be balanced by a call to {@link shutdown()}.
   */
  static bool startup(Error** outError = nullptr);

  /**
   * Releases one reference of the Media Foundation platform.
   */
  static void shutdown();

  /**
   * Converts a UTF-8 string to a UTF-16 string.
   */
  static std::wstring utf8ToWide(const std::string& value);

  /**
   * Converts a UTF-16 string to a UTF-8 string.
   */
  static std::string wideToUtf8(const std::wstring& value);

  /**
   * Converts an output file path or URL to the wide string a sink writer is created from.
   *
   * @discussion
   * `MFCreateSinkWriterFromURL` parses its argument as a URL and refuses a relative path with
   * E_ACCESSDENIED, which is what a caller that passes `input.mp4` from the working directory hits.
   * A value without a URL scheme is therefore resolved to an absolute path first; a value that names
   * a scheme is handed over unchanged.
   */
  static std::wstring toSinkWriterUrl(const std::string& url);

  /**
   * Stores the error of the HRESULT to the outError.
   */
  static void handleHresult(Error** outError,
    HRESULT hr,
    const char* errorMsg,
    const std::string& description = {});

  /**
   * Returns the readable text of the HRESULT.
   */
  static std::string hresultToString(HRESULT hr);

  // ------------------------------------------------------------------------------------------------
  // Audio/video type conversions
  // ------------------------------------------------------------------------------------------------

  static bool audioFormatTypeToSubtype(AudioFormatType type, GUID& subType);

  static bool videoCodecTypeToSubtype(VideoCodecType type, GUID& subType);

  static AudioFormatType subtypeToAudioFormatType(const GUID& subType);

  static VideoCodecType subtypeToVideoCodecType(const GUID& subType);

  /**
   * Returns the coral mime type of the Media Foundation sub type, empty when unknown.
   */
  static std::string subtypeToMimeType(const GUID& subType);

  /**
   * Returns the Media Foundation sub type of the coral mime type.
   */
  static bool mimeTypeToSubtype(const std::string& mimeType, GUID& subType);

  static AudioSampleFormat subtypeToSampleFormat(const GUID& subType);

  static bool sampleFormatToSubtype(AudioSampleFormat format, GUID& subType);

  static int bitsPerSample(AudioSampleFormat format);

  // ------------------------------------------------------------------------------------------------
  // Media Foundation transforms
  // ------------------------------------------------------------------------------------------------

  /**
   * Returns the category of the transform.
   */
  static const GUID& transformCategory(bool video, bool decoder);

  /**
   * Indicates whether a synchronous transform is available for the codec.
   *
   * @param video    Whether the codec is a video codec.
   * @param subType  The compressed media sub type of the codec.
   * @param decoder  Whether the transform is a decoder.
   */
  static bool isTransformAvailable(bool video, const GUID& subType, bool decoder);

  /**
   * Creates a synchronous transform for the codec.
   *
   * @param video    Whether the codec is a video codec.
   * @param subType  The compressed media sub type of the codec.
   * @param decoder  Whether the transform is a decoder.
   */
  static Microsoft::WRL::ComPtr<IMFTransform> createTransform(bool video, const GUID& subType, bool decoder);

  // ------------------------------------------------------------------------------------------------
  // Media types
  // ------------------------------------------------------------------------------------------------

  /**
   * Creates a raw PCM audio media type.
   */
  static Microsoft::WRL::ComPtr<IMFMediaType> createPcmMediaType(const GUID& subType,
    int sampleRate,
    int channelCount);

  /**
   * Wraps the data into an IMFSample.
   */
  static Microsoft::WRL::ComPtr<IMFSample> createSample(const uint8_t* data,
    size_t size,
    int64_t timeUs,
    uint32_t flags = 0);

  /**
   * Sets the coral mime type, csd and the shared properties of the media type.
   */
  static void fillMediaFormat(IMFMediaType* mediaType,
    const std::shared_ptr<MediaFormat>& format,
    bool compressed);

  /**
   * Applies the shared properties (sample rate, channel count, bitrate ...) of the
   * coral format on the media type.
   */
  static void fillAudioMediaType(IMFMediaType* mediaType,
    const std::shared_ptr<MediaFormat>& format,
    const GUID& subType);

  static void fillVideoMediaType(IMFMediaType* mediaType,
    const std::shared_ptr<MediaFormat>& format,
    const GUID& subType);

  /**
   * Converts a Media Foundation media type to a coral MediaFormat.
   *
   * @param mediaType   The Media Foundation media type.
   * @param compressed  Whether the media type describes compressed data, so that the
   *                    codec specific data is extracted into the format.
   */
  static std::shared_ptr<MediaFormat>
  convertToMediaFormat(IMFMediaType* mediaType, bool compressed);

  /**
   * Returns the pixel format of the Media Foundation video sub type.
   */
  static PixelFormat subtypeToPixelFormat(const GUID& subType);

  /**
   * Builds the MF_MT_USER_DATA blob of an AAC media type. The first 12 bytes are the part of the
   * HEAACWAVEFORMAT structure which follows the WAVEFORMATEX structure, followed by the
   * AudioSpecificConfig.
   */
  static std::vector<uint8_t> createAacUserData(const std::vector<uint8_t>& audioSpecificConfig);

private:
  WmfMediaUtils() = default;
  ~WmfMediaUtils() = default;
};

/**
 * A scoped helper which keeps the media foundation platform alive for the enclosing scope.
 */
class WmfMediaScope final {
public:
  WmfMediaScope()
    : mStarted(WmfMediaUtils::startup()) {
  }

  ~WmfMediaScope() {
    if (mStarted) {
      WmfMediaUtils::shutdown();
    }
  }

  WmfMediaScope(const WmfMediaScope&) = delete;
  WmfMediaScope& operator=(const WmfMediaScope&) = delete;

  [[nodiscard]] bool isStarted() const {
    return mStarted;
  }

private:
  bool mStarted;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_MEDIA_UTILS_H
