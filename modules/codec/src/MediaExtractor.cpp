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

#include <codec/MediaExtractor.h>

#if defined(ANDROID) || defined(__ANDROID__)

#include "android/MediaExtractor.h"
#include "android/NativeExtractor.h"

#endif

#if defined(WINDOWS) || defined(WIN32)

#include "win/WmfMediaExtractor.h"

#endif

namespace coral {

AVMediaExtractor::~AVMediaExtractor() noexcept = default;

int AVMediaExtractor::sniff(AVExtractorInput& input) {
  static_cast<void>(input);
  // The extractor of a platform is created for the media it reads, so it recognizes what it was given:
  // whether it could open the media at all is what the factory reports, by returning null. An extractor
  // that is created for an input of the reader opens the media here, and overrides this.
  return AVExtractorsFactory::kSniffResultPossible;
}

std::shared_ptr<AVMediaExtractor> ExtractorFactory::createInputExtractor(const std::string& url) {
  static_cast<void>(url);
  // The platform has no extractor that reads the input of the reader: its extractor is created for the
  // url of the media, and the caller has to drive it without measuring the bytes of its own input.
  return nullptr;
}

AVExtractor::Result AVMediaExtractor::read(AVExtractorInput& input) {
  switch (read(platformPosition_)) {
    case PlatformResult::RESULT_END_OF_INPUT:
      return AVExtractor::kResultEndOfInput;

    case PlatformResult::RESULT_SEEK: {
      // The extractor asks for the media to be read at another position, which is a seek of the input
      // of the reader. The time of that position is not known: the media of a platform extractor is
      // read by the extractor itself, so the seek it is told about is the one it makes of its own
      // reader. Every extractor of this module seeks internally and reports the end of its media
      // rather than RESULT_SEEK, therefore the time is unset here.
      const auto position = static_cast<long>(platformPosition_.getPosition());
      if (!input.resetPosition(position, true)) {
        return AVExtractor::kResultEndOfInput;
      }
      seek(position, Definitions::kTimeUnset);
      return AVExtractor::kResultContinue;
    }

    case PlatformResult::RESULT_CONTINUE:
    default:
      return AVExtractor::kResultContinue;
  }
}

bool AVMediaExtractor::seek(const long position, const int64_t timeUs) {
  static_cast<void>(position);
  return seekTo(timeUs, SeekOption::SEEK_TO_PREVIOUS_SYNC);
}

const std::vector<AVSeekMap::SeekPoint>& AVMediaExtractor::getSeekPoints() const {
  // The seek points of a time are resolved on demand by the platforms, which do not enumerate them.
  static const std::vector<SeekPoint> sEmpty;
  return sEmpty;
}

bool AVMediaExtractor::getSeekPoints(const int64_t timeUs, SeekPoint& first, SeekPoint& second) const {
  if (!isSeekable()) {
    return false;
  }
  const coral::SeekPoints points = getPlatformSeekPoints(timeUs);
  first = SeekPoint(points.first.timeUs, static_cast<long>(points.first.position));
  second = SeekPoint(points.second.timeUs, static_cast<long>(points.second.position));
  return true;
}

namespace {

/**
 * The default extractor factory implementation
 */
class BuiltinExtractorFactory final : public ExtractorFactory {
public:
  ~BuiltinExtractorFactory() override = default;

  [[nodiscard]] std::shared_ptr<AVMediaExtractor>
  createExtractor(const std::string& url, bool preferredPlatform) override;

  std::shared_ptr<AVMediaExtractor> createInputExtractor(const std::string& url) override;

  static std::shared_ptr<AVMediaExtractor> createPlatformExtractor(const std::string& url);
};

std::shared_ptr<AVMediaExtractor>
BuiltinExtractorFactory::createExtractor(const std::string& url, const bool preferredPlatform) {
  std::shared_ptr<AVMediaExtractor> extractor = nullptr;

  if (preferredPlatform) {
    extractor = createPlatformExtractor(url);
    if (extractor != nullptr) {
      return extractor;
    }
  }

  return createPlatformExtractor(url);
}

std::shared_ptr<AVMediaExtractor>
BuiltinExtractorFactory::createPlatformExtractor(const std::string& url) {
#if defined(ANDROID) || defined(__ANDROID__)
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
  auto extractor = NativeExtractor::CreateWithUrl(url);
  if (extractor) {
    return extractor;
  }
#endif // CORAL_SUPPORTS_NDK_MEDIACODEC
  return MediaExtractor::CreateWithUrl(url);
#elif defined(WINDOWS) || defined(WIN32)

  // the Windows media foundation source reader based extractor
  return WmfMediaExtractor::createWithUrl(url);

#else

  // unknown platform implementation
  return nullptr;

#endif
}

std::shared_ptr<AVMediaExtractor>
BuiltinExtractorFactory::createInputExtractor(const std::string& url) {
#if defined(ANDROID) || defined(__ANDROID__)
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)

  // The extractor of the NDK reads the media of the reader through the custom data source of the
  // platform, see NativeExtractor. A platform that predates that data source reports it by returning
  // null, and the caller then drives an extractor that opens the media of the url.
  return NativeExtractor::CreateForInput(url);

#else

  // The extractor of the platform of this build is the one of the Java framework, which reads the media
  // of the url it is given: the reader cannot measure the bytes it consumed.
  static_cast<void>(url);
  return nullptr;

#endif // CORAL_SUPPORTS_NDK_MEDIACODEC
#elif defined(WINDOWS) || defined(WIN32)

  // The source reader of the media foundation is created over the input of the reader, see
  // WmfExtractorByteStream, so the extractor reads the media the reader owns.
  return WmfMediaExtractor::createForInput(url);

#else

  // The extractor of the platform of this build is opened by the url of the media: it reads the media
  // itself, so the reader cannot measure the bytes it consumed, see PlatformProgressiveMediaExtractor.
  static_cast<void>(url);
  return nullptr;

#endif
}

} // namespace anonymous

std::unique_ptr<ExtractorFactory> CreateBuiltinExtractorFactory() {
  return std::make_unique<BuiltinExtractorFactory>();
}

} // namespace coral
