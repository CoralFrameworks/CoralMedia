cmake_minimum_required(VERSION 3.19)
set(TARGET test_muxer)

add_executable(${TARGET}
    test_main.cpp
    test_Containers.cpp
    test_Mp4Muxer.cpp
    test_SampleTable.cpp
)

target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(${TARGET} PRIVATE muxer extractor datasource gtest)
target_compile_definitions(${TARGET} PRIVATE CORAL_ASSETS_DIR="${CORAL_TEST_DATA_DIR}")
set_target_properties(${TARGET} PROPERTIES FOLDER Tests)

add_test(NAME ${TARGET} COMMAND ${TARGET})
