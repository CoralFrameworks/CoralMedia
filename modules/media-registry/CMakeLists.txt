cmake_minimum_required(VERSION 3.19)
set(TARGET media-registry)
set(PUBLIC_HDR_DIR include)
set(PRIVATE_HDR_DIR src)

# The registry that lets an application (or a third party) install its own codec, extractor and
# muxer factories. It is deliberately separate from `media` (the data model): the header is
# <media/registry/MediaEngine.h>, so the old `media/MediaEngine.h` name is gone before the
# repositories are split.
set(PUBLIC_HDRS
    include/media/registry/MediaEngine.h
)

set(PRIVATE_HDRS
    src/FrameworkAudioCodecFactory.h
    src/FrameworkExtractorFactory.h
    src/FrameworkMuxerFactory.h
    src/FrameworkVideoCodecFactory.h
)

set(SRCS
    src/FrameworkAudioCodecFactory.cpp
    src/FrameworkExtractorFactory.cpp
    src/FrameworkMuxerFactory.cpp
    src/FrameworkVideoCodecFactory.cpp
    src/MediaEngine.cpp
)

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})

target_include_directories(${TARGET}
    PUBLIC ${PUBLIC_HDR_DIR}
    PRIVATE ${PRIVATE_HDR_DIR}
)

# The registry has no audio runtime dependency: the resampler it used to re-export moved to CoralAudio
# with the rest of the audio stack, and CoralAudio depends on CoralMedia rather than the other way
# around. Naming `audio` here would make that dependency a cycle.
target_link_libraries(${TARGET} PRIVATE codec extractor minilog)

# The framework factories compile their FFmpeg branch from the definition that ffcodec publishes.
if (CORAL_SUPPORTS_FFMPEG)
    target_link_libraries(${TARGET} PRIVATE ffcodec)
endif ()

set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/media DESTINATION include)
