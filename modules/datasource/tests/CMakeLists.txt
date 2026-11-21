cmake_minimum_required(VERSION 3.19)
set(TARGET test_datasource)

# ==================================================================================================
# Tests
# ==================================================================================================

add_executable(${TARGET}
    TestData.h
    test_main.cpp
    test_DataSource.cpp
    test_HttpDataSource.cpp
    test_HttpDataSourceBase.cpp
)

target_link_libraries(${TARGET} PRIVATE datasource gtest)

# The strategy of the http reads is the one of the module rather than the one of a platform, therefore
# the suite binds it to a server of its own and includes the private header that declares it.
target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set_target_properties(${TARGET} PROPERTIES FOLDER Tests)

add_test(NAME ${TARGET} COMMAND ${TARGET})
