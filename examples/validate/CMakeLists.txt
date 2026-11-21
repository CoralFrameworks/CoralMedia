# ==================================================================================================
# coralmedia_validate
# ==================================================================================================
# One executable, one stage per check: it encodes synthetic audio and video, muxes the encoded tracks,
# demuxes the file it wrote and decodes the demuxed tracks. Every stage prints PASS, SKIP or FAIL and
# the process exit code is 0 when nothing failed, 1 when a stage failed and 77 when nothing could be
# validated (no platform codec and no input file), which CTest reports as skipped.

add_executable(coralmedia_validate main.cpp)

target_link_libraries(coralmedia_validate PRIVATE
    codec
    datasource
    extractor
    media
    muxer
    minilog
)

if (MSVC)
  # The Windows codec headers include windows.h, whose min/max macros conflict with std::min/std::max.
  target_compile_definitions(coralmedia_validate PRIVATE NOMINMAX)
endif ()

set_target_properties(coralmedia_validate PROPERTIES FOLDER Examples)

# A checkout that carries fixtures runs the demux and decode stages on a real file as well; one that
# does not still validates the full encode -> mux -> demux -> decode round trip.
set(_coralmedia_fixture "")
if (DEFINED CORAL_TEST_DATA_DIR AND EXISTS "${CORAL_TEST_DATA_DIR}/video/video720p_5s.mp4")
  set(_coralmedia_fixture --input "${CORAL_TEST_DATA_DIR}/video/video720p_5s.mp4")
endif ()

add_test(NAME coralmedia_validate
    COMMAND coralmedia_validate --workdir "${CMAKE_CURRENT_BINARY_DIR}/work" ${_coralmedia_fixture})

set_tests_properties(coralmedia_validate PROPERTIES SKIP_RETURN_CODE 77)
