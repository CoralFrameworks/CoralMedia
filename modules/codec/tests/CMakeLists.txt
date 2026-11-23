# ==================================================================================================
# Tests
#
# Each test is a standalone executable named tests_<Thing>, registered with CTest.
# ==================================================================================================

set(TEST_TARGET tests_codec)

set(TEST_SRCS
    tests_codec.cpp
    tests_AudioFile.cpp
    tests_AudioCodec.cpp
    tests_MediaExtractor.cpp
    tests_MuxerAndMetadata.cpp
    tests_VideoCodec.cpp
    tests_HardwareEncoder.cpp
    CodecTestUtils.h
)

if (APPLE)
  # The Apple suites are the ones of the Darwin platform layer. The Windows suites are compiled out
  # by their own platform guard, so only this file has to be selected here.
  list(APPEND TEST_SRCS tests_DarwinCodec.cpp)
endif ()

add_executable(${TEST_TARGET} ${TEST_SRCS})

if(WIN32)
  target_link_libraries(${TEST_TARGET} PRIVATE d3d11 dxgi)
endif()

if(APPLE)
  # The zero-copy test reads the pixel buffer and its IOSurface directly.
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(${TEST_TARGET} PRIVATE
      ${COREVIDEO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  # The bitstream helper suite includes the platform header of the codec.
  target_include_directories(${TEST_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
  # A checkout that names its fixtures runs the real media suite; without it the test skips.
  if (DEFINED CORAL_TEST_DATA_DIR)
    target_compile_definitions(${TEST_TARGET} PRIVATE
        CORAL_TEST_DATA_DIR="${CORAL_TEST_DATA_DIR}")
  endif ()
endif()

target_link_libraries(${TEST_TARGET} PRIVATE codec)
target_link_libraries(${TEST_TARGET} PRIVATE gtest)
target_link_libraries(${TEST_TARGET} PRIVATE minilog)

if (MSVC)
  # the windows media foundation headers are used by the tests.
  target_compile_definitions(${TEST_TARGET} PRIVATE NOMINMAX)
endif ()

set_target_properties(${TEST_TARGET} PROPERTIES FOLDER Tests)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
