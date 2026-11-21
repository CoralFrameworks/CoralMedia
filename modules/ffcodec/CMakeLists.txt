cmake_minimum_required(VERSION 3.19)
set(TARGET ffcodec)
set(PUBLIC_HDR_DIR include)

# ==================================================================================================
# Sources and headers
# ==================================================================================================

set(PUBLIC_HDRS
    include/ffcodec/FfmpegAudioCodecFactory.h
    include/ffcodec/FfmpegAudioDecoder.h
    include/ffcodec/FfmpegAudioEncoder.h
    include/ffcodec/FfmpegAudioFileReader.h
    include/ffcodec/FfmpegExtractor.h
    include/ffcodec/FfmpegMetadataRetriever.h
    include/ffcodec/FfmpegMuxer.h
    include/ffcodec/FfmpegVideoCodecFactory.h
    include/ffcodec/FfmpegVideoDecoder.h
    include/ffcodec/FfmpegVideoEncoder.h
)

set(SRCS
    src/FfmpegAudioCodecFactory.cpp
    src/FfmpegAudioDecoder.cpp
    src/FfmpegAudioEncoder.cpp
    src/FfmpegAudioFileReader.cpp
    src/FfmpegExtractor.cpp
    src/FfmpegMetadataRetriever.cpp
    src/FfmpegMuxer.cpp
    src/FfmpegUtils.cpp
    src/FfmpegVideoCodecFactory.cpp
    src/FfmpegVideoDecoder.cpp
    src/FfmpegVideoEncoder.cpp
    src/SimpleAudioDecoder.cpp
    src/SimpleVideoDecoder.cpp
)

set(PRIVATE_HDRS
    include/private/ffcodec/FfmpegUtils.h
    include/private/ffcodec/SimpleAudioDecoder.h
    include/private/ffcodec/SimpleDecoder.h
    include/private/ffcodec/SimpleVideoDecoder.h
)

# ==================================================================================================
# Includes & target definition
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})

add_library(${TARGET} STATIC ${PRIVATE_HDRS} ${PUBLIC_HDRS} ${SRCS})

target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

target_link_libraries(${TARGET} PUBLIC codec)
target_link_libraries(${TARGET} PRIVATE minilog)

# FFmpeg and libyuv are resolved by cmake/CoralMediaDependencies.cmake before this module is added;
# naming them here is what puts them in the link line. libyuv converts between the planar formats FFmpeg
# produces and the NV12/I420 buffers the framework's video path uses. The definition is PUBLIC because
# media-registry's framework factories compile their FFmpeg branch from it.
if (CORAL_SUPPORTS_FFMPEG)
  target_link_libraries(${TARGET} PUBLIC FFmpeg::ffmpeg)
  target_link_libraries(${TARGET} PRIVATE yuv)
  target_compile_definitions(${TARGET} PUBLIC CORAL_SUPPORTS_FFMPEG)
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Test
# ==================================================================================================
