cmake_minimum_required(VERSION 3.19)
set(TARGET test_datasource)

# ==================================================================================================
# Tests
# ==================================================================================================

set(TEST_SRCS
    TestData.h
    test_main.cpp
    test_DataSource.cpp
    test_HttpDataSource.cpp
    test_HttpDataSourceBase.cpp
)

if (APPLE)
  # The Darwin http suite serves its media from a socket of the test, i.e. from a thread of its own.
  list(APPEND TEST_SRCS test_DarwinHttpDataSource.cpp)
  find_package(Threads REQUIRED)
endif ()

add_executable(${TARGET} ${TEST_SRCS})

target_link_libraries(${TARGET} PRIVATE datasource gtest)

if (APPLE)
  # The bundle resource suite reads the resources directory of the main bundle with CoreFoundation,
  # which keeps the test a C++ translation unit rather than a second Objective-C++ one.
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(${TARGET} PRIVATE ${COREFOUNDATION_FRAMEWORK})
  target_link_libraries(${TARGET} PRIVATE Threads::Threads)
endif ()

# The strategy of the http reads is the one of the module rather than the one of a platform, therefore
# the suite binds it to a server of its own and includes the private header that declares it.
target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set_target_properties(${TARGET} PROPERTIES FOLDER Tests)

add_test(NAME ${TARGET} COMMAND ${TARGET})
