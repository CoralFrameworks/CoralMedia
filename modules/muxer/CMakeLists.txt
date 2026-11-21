cmake_minimum_required(VERSION 3.19)
set(TARGET muxer)
set(PUBLIC_HDR_DIR include)

set(PUBLIC_HDRS
    include/muxer/Muxer.h
    include/muxer/MuxerAdapter.h
    include/muxer/MuxerFactory.h
    include/muxer/MuxerTypes.h
)

set(SRCS
    src/AacMuxer.cpp
    src/BoxWriter.cpp
    src/FileMuxerOutput.cpp
    src/Muxer.cpp
    src/MuxerAdapter.cpp
    src/MuxerFactory.cpp
    src/WavMuxer.cpp
    src/mp4/FragmentedMp4Muxer.cpp
    src/mp4/Mp4Boxes.cpp
    src/mp4/Mp4Metadata.cpp
    src/mp4/Mp4Muxer.cpp
    src/mp4/Mp4SampleEntry.cpp
    src/mp4/SampleTable.cpp
    src/ogg/OggMuxer.cpp
    src/webm/Ebml.cpp
    src/webm/WebmMuxer.cpp
)

include_directories(${PUBLIC_HDR_DIR})
include_directories(src)

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
target_link_libraries(${TARGET} PUBLIC codec media utils minilog)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

if (MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif ()

install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

if (CORAL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif ()
