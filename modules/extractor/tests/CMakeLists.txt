cmake_minimum_required(VERSION 3.19)
set(TARGET test_extractor)

# ==================================================================================================
# Tests
# ==================================================================================================

add_executable(${TARGET}
    TestMedia.h
    test_main.cpp
    test_ContainerSubtitles.cpp
    test_CueModel.cpp
    test_CueTimeline.cpp
    test_DataBuffer.cpp
    test_ExtractorInput.cpp
    test_Format.cpp
    test_Id3Parser.cpp
    test_MetadataContainers.cpp
    test_MetadataDecoder.cpp
    test_VorbisCommentParser.cpp
    test_Reader.cpp
    test_SsaParser.cpp
    test_SubripParser.cpp
  test_TtmlParser.cpp
  test_PgsParser.cpp
  test_VobSubParser.cpp
  test_DvbSubParser.cpp
  test_Cea608Parser.cpp
  test_Cea708Parser.cpp
  test_Tx3gParser.cpp
    test_SubtitleParserFactory.cpp
    test_Utility.cpp
    test_WebvttParser.cpp
)

target_link_libraries(${TARGET} PRIVATE extractor datasource gtest)

# The container fixtures (a Matroska file with a subtitle track) are committed under `assets`.
target_compile_definitions(${TARGET} PRIVATE
    CORAL_ASSETS_DIR="${CORAL_TEST_DATA_DIR}"
)

set_target_properties(${TARGET} PROPERTIES FOLDER Tests)

add_test(NAME ${TARGET} COMMAND ${TARGET})
