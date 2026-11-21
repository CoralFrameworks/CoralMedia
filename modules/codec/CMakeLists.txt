cmake_minimum_required(VERSION 3.19)
set(TARGET codec)
set(PUBLIC_HDR_DIR include)

# ==================================================================================================
# Sources and headers
# ==================================================================================================
set(PUBLIC_HDRS
    include/codec/AudioFileReader.h
    include/codec/AudioFileWriter.h
    include/codec/CodecTypes.h
    include/codec/Error.h
    include/codec/HardwareVideo.h
    include/codec/MediaCodec.h
    include/codec/MediaCodecFactory.h
    include/codec/MediaExtractor.h
    include/codec/MediaMetadataRetriever.h
    include/codec/MediaMuxer.h
    include/codec/Utilities.h
)

set(SRCS
    src/common/AudioFileReader.cpp
    src/common/AudioFileWriter.cpp
    src/common/AudioMuxFileWriter.cpp
    src/common/AudioMuxFileWriter.h
    src/common/ExtractorMetadataRetriever.cpp
    src/common/ExtractorMetadataRetriever.h
    src/common/WaveFileReader.cpp
    src/common/WaveFileReader.h
    src/common/WaveFileWriter.cpp
    src/common/WaveFileWriter.h
    src/AudioCodec.cpp
    src/CodecTypes.cpp
    src/MediaExtractor.cpp
    src/MediaMetadataRetriever.cpp
    src/MediaMuxer.cpp
    src/PcmAudioDecoder.cpp
    src/PcmAudioDecoder.h
    src/Utilities.cpp
    src/VideoCodec.cpp
)

# ==================================================================================================
# Platform code sources
# ==================================================================================================

if (ANDROID)
  list(APPEND SRCS
      src/android/MediaCodecAudioDecoder.cpp
      src/android/MediaCodecAudioDecoder.h
      src/android/MediaCodecAudioEncoder.cpp
      src/android/MediaCodecAudioEncoder.h
      src/android/MediaCodecBuffer.h
      src/android/MediaCodecSurface.h
      src/android/MediaCodecUtils.cpp
      src/android/MediaCodecUtils.h
      src/android/MediaCodecVideoDecoder.cpp
      src/android/MediaCodecVideoDecoder.h
      src/android/MediaCodecVideoEncoder.cpp
      src/android/MediaCodecVideoEncoder.h
      src/android/MediaExtractor.cpp
      src/android/MediaExtractor.h
      src/android/MediaMuxer.cpp
      src/android/MediaMuxer.h
      src/android/NativeAudioDecoder.cpp
      src/android/NativeAudioDecoder.h
      src/android/NativeAudioEncoder.cpp
      src/android/NativeAudioEncoder.h
      src/android/NativeExtractor.cpp
      src/android/NativeExtractor.h
      src/android/NativeMuxer.cpp
      src/android/NativeMuxer.h
      src/android/NativeSurface.cpp
      src/android/NativeSurface.h
      src/android/NativeVideoDecoder.cpp
      src/android/NativeVideoDecoder.h
      src/android/NativeVideoEncoder.cpp
      src/android/NativeVideoEncoder.h
      src/android/NdkAudioFileReader.cpp
      src/android/NdkAudioFileReader.h
  )
endif ()


if (WIN32)
  list(APPEND SRCS
      src/win/WmfAudioDecoder.cpp
      src/win/WmfAudioDecoder.h
      src/win/WmfAudioEncoder.cpp
      src/win/WmfAudioEncoder.h
      src/win/WmfAudioFileReader.cpp
      src/win/WmfAudioFileReader.h
      src/win/WmfAudioFileWriter.cpp
      src/win/WmfAudioFileWriter.h
      src/win/WmfExtractorByteStream.cpp
      src/win/WmfExtractorByteStream.h
      src/win/WmfMediaExtractor.cpp
      src/win/WmfMediaExtractor.h
      src/win/WmfMediaMuxer.cpp
      src/win/WmfMediaMuxer.h
      src/win/WmfMediaUtils.cpp
      src/win/WmfMediaUtils.h
      src/win/WmfTransform.cpp
      src/win/WmfTransform.h
      src/win/WmfHardwarePixelBuffer.cpp
      src/win/WmfHardwarePixelBuffer.h
      src/win/WmfHardwareVideoEncoder.cpp
      src/win/WmfHardwareVideoEncoder.h
      src/win/WmfVideoDecoder.cpp
      src/win/WmfVideoDecoder.h
      src/win/WmfVideoEncoder.cpp
      src/win/WmfVideoEncoder.h
  )
endif ()

# ==================================================================================================
# Include and target definitions
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})
include_directories(src)

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})

target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

target_link_libraries(${TARGET} PUBLIC media)
# The interface of a platform extractor is the interface of the extractor module, so that the extractor of
# a platform and a built-in one are interchangeable.
target_link_libraries(${TARGET} PUBLIC extractor)
target_link_libraries(${TARGET} PRIVATE minilog)

if (ANDROID)
  target_link_libraries(${TARGET} PRIVATE android jni mediandk)
endif ()

if (WIN32)
  # The hardware output path views and shares ID3D11Texture2D, whose IIDs are in d3d11.lib.
  target_link_libraries(${TARGET} PRIVATE mfplat mfreadwrite mfuuid mf mfcore ole32 d3d11)
endif ()

if (WIN32)
  # The media foundation headers include windows.h which defines the min/max macros, they conflict
  # with the std::min/std::max usage of the implementation.
  add_definitions(-DNOMINMAX)
endif ()

# ==================================================================================================
# Compiler flags
# ==================================================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CYGWIN)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif ()
if (MSVC)
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:/fp:fast>)
else ()
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:-ffast-math>)
  target_compile_options(${TARGET} PRIVATE -Wno-deprecated-register)
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Samples and tests
# ==================================================================================================
if (IS_HOST_PLATFORM)
  add_subdirectory(samples)
endif ()

# The suites link GoogleTest, so they follow the framework-wide test switch rather than only the
# platform one; the samples above are plain programs and need neither.
if (CORAL_BUILD_TESTS AND IS_HOST_PLATFORM)
  enable_testing()
  add_subdirectory(tests)
endif ()


