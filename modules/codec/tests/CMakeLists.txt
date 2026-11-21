# ==================================================================================================
# Tests
#
# Each test is a standalone executable named tests_<Thing>, registered with CTest.
# ==================================================================================================

set(TEST_TARGET tests_codec)

add_executable(${TEST_TARGET}
    tests_codec.cpp
    tests_AudioFile.cpp
    tests_AudioCodec.cpp
    tests_MediaExtractor.cpp
    tests_MuxerAndMetadata.cpp
    tests_VideoCodec.cpp
    tests_HardwareEncoder.cpp
    CodecTestUtils.h
)

if(WIN32)
  target_link_libraries(${TEST_TARGET} PRIVATE d3d11 dxgi)
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
