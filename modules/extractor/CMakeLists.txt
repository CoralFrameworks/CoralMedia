cmake_minimum_required(VERSION 3.19)
set(TARGET extractor)
set(PUBLIC_HDR_DIR include)

# ==================================================================================================
# Sources and headers
# ==================================================================================================
# The module builds a single library: it holds the interfaces, the readers, the helper classes, the
# audio only containers, the audio/video containers and the still image containers.

set(PUBLIC_HDRS
    include/extractor/DataBuffer.h
    include/extractor/DefaultExtractorInput.h
    include/extractor/DefaultExtractorsFactory.h
    include/extractor/Extractor.h
    include/extractor/ExtractorInput.h
    include/extractor/MediaExtractorInput.h
    include/extractor/Reader.h
    include/extractor/Utility.h
    include/extractor/audio/AdtsExtractor.h
    include/extractor/audio/AdtsReader.h
    include/extractor/audio/AmrExtractor.h
    include/extractor/audio/FlacExtractor.h
    include/extractor/audio/Mp3Extractor.h
    include/extractor/audio/MpegAudio.h
    include/extractor/audio/WavExtractor.h
    include/extractor/container/AviExtractor.h
    include/extractor/container/FlvExtractor.h
    include/extractor/container/MatroskaExtractor.h
    include/extractor/container/Mp4Extractor.h
    include/extractor/container/OggExtractor.h
    include/extractor/container/TsExtractor.h
    include/extractor/image/AvifExtractor.h
    include/extractor/image/BmpExtractor.h
    include/extractor/image/HeifExtractor.h
    include/extractor/image/JpegExtractor.h
    include/extractor/image/PngExtractor.h
    include/extractor/image/SingleSampleExtractor.h
    include/extractor/image/WebpExtractor.h
    include/extractor/metadata/Id3Parser.h
    include/extractor/metadata/MetadataDecoder.h
    include/extractor/metadata/VorbisCommentParser.h
    include/extractor/text/Cue.h
    include/extractor/text/CueGroup.h
    include/extractor/text/CueTimeline.h
    include/extractor/text/CuesWithTiming.h
    include/extractor/text/DefaultSubtitleParserFactory.h
    include/extractor/text/SubtitleParser.h
    include/extractor/text/ssa/SsaParser.h
    include/extractor/text/subrip/SubripParser.h
    include/extractor/text/tx3g/Tx3gParser.h
    include/extractor/text/ttml/TtmlParser.h
    include/extractor/text/pgs/PgsParser.h
    include/extractor/text/vobsub/VobSubParser.h
    include/extractor/text/dvbsub/DvbSubParser.h
    include/extractor/text/cea608/Cea608Parser.h
    include/extractor/text/cea708/Cea708Parser.h
    include/extractor/text/cea708/Cea708Sei.h
    include/extractor/text/SubtitleExtractor.h
    include/extractor/text/webvtt/Mp4WebvttParser.h
    include/extractor/text/webvtt/WebvttParser.h
)

set(SRCS
    src/DefaultExtractorInput.cpp
    src/DefaultExtractorsFactory.cpp
    src/Reader.cpp
    src/Utility.cpp
    src/audio/AdtsExtractor.cpp
    src/audio/AdtsReader.cpp
    src/audio/AmrExtractor.cpp
    src/audio/FlacExtractor.cpp
    src/audio/Mp3Extractor.cpp
    src/audio/MpegAudio.cpp
    src/audio/WavExtractor.cpp
    src/container/AviExtractor.cpp
    src/container/FlvExtractor.cpp
    src/container/MatroskaExtractor.cpp
    src/container/Mp4Extractor.cpp
    src/container/OggExtractor.cpp
    src/container/TsExtractor.cpp
    src/image/ImageExtractors.cpp
    src/image/SingleSampleExtractor.cpp
    src/metadata/Id3Parser.cpp
    src/metadata/MetadataDecoder.cpp
    src/metadata/VorbisCommentParser.cpp
    src/text/Cue.cpp
    src/text/CueGroup.cpp
    src/text/CuesWithTiming.cpp
    src/text/CueTimeline.cpp
    src/text/DefaultSubtitleParserFactory.cpp
    src/text/SubtitleParser.cpp
    src/text/SubtitleTextUtil.cpp
    src/text/SubtitleTextUtil.h
    src/text/ssa/SsaParser.cpp
    src/text/subrip/SubripParser.cpp
    src/text/tx3g/Tx3gParser.cpp
    src/text/ttml/TtmlParser.cpp
    src/text/pgs/PgsParser.cpp
    src/text/vobsub/VobSubParser.cpp
    src/text/dvbsub/DvbSubParser.cpp
    src/text/cea608/Cea608Parser.cpp
    src/text/cea708/Cea708Parser.cpp
    src/text/cea708/Cea708Sei.cpp
    src/text/SubtitleExtractor.cpp
    src/text/webvtt/Mp4WebvttParser.cpp
    src/text/webvtt/WebvttCss.cpp
    src/text/webvtt/WebvttCss.h
    src/text/webvtt/WebvttCueParser.cpp
    src/text/webvtt/WebvttCueParser.h
    src/text/webvtt/WebvttParser.cpp
)

# ==================================================================================================
# Include and target definitions
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})
include_directories(src)

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
target_link_libraries(${TARGET} PUBLIC datasource media minilog utils z)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# ==================================================================================================
# Compiler flags
# ==================================================================================================
if (MSVC)
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:/fp:fast>)
else ()
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:-ffast-math -fno-finite-math-only>)
  target_compile_options(${TARGET} PRIVATE -Wno-deprecated-register)
endif ()
target_compile_features(${TARGET} PRIVATE cxx_strong_enums)

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Tests and demo
# ==================================================================================================
if (CORAL_BUILD_TESTS)

  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(demo)

endif ()
