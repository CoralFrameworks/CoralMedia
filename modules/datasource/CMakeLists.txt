cmake_minimum_required(VERSION 3.19)
set(TARGET datasource)
set(PUBLIC_HDR_DIR include)

# ==================================================================================================
# Sources and headers
# ==================================================================================================
# The module holds the data sources that read the bytes of a media, i.e. the counterpart of the
# ExoPlayer data sources: a file of the local file system, an in memory buffer, the uri of either of
# them, and an http or an https uri.
#
# The http data source is the only one that is implemented by a platform: the interface is common, and
# the file that talks to a server belongs to the platform. HttpDataSourceBase holds what does not
# depend on a platform, i.e. the window the reads are served from.

set(PUBLIC_HDRS
    include/datasource/AndroidDataSource.h
    include/datasource/DataSource.h
    include/datasource/DarwinDataSource.h
    include/datasource/HttpDataSource.h
)

set(PRIVATE_HDRS
    src/HttpDataSourceBase.h
)

set(SRCS
    src/DataSource.cpp
    src/HttpDataSourceBase.cpp
)

# ==================================================================================================
# Platform sources
# ==================================================================================================

if (WIN32)
  # The media of an http uri is read with WinHTTP, the http API of the platform.
  list(APPEND SRCS
      src/win32/WinHttpDataSource.cpp
  )
elseif (APPLE)
  # The media of an http uri is read with NSURLSession, the http API of the platform.
  list(APPEND SRCS
      src/darwin/DarwinHttpDataSource.mm
  )
else ()
  list(APPEND SRCS
      src/UnsupportedHttpDataSource.cpp
  )
endif ()

if (ANDROID)
  # The assets, the raw resources and the media of a content provider are read through the platform,
  # which is reached through the jni module: an asset or a resource is a region of the APK, and the
  # media of a content uri is a region of the file a provider hands out.
  list(APPEND PRIVATE_HDRS
      src/android/AndroidDataSourceSupport.h
  )
  list(APPEND SRCS
      src/android/AndroidDataSourceSupport.cpp
      src/android/AssetDataSource.cpp
      src/android/ContentDataSource.cpp
      src/android/FileDescriptorDataSource.cpp
      src/android/RawResourceDataSource.cpp
  )
endif ()

if (APPLE)
  # The file of the local file system and the resource of the main bundle are read with Foundation:
  # a url the system picker returned is security scoped, and a bundle resource is looked up rather
  # than named by a path.
  list(APPEND SRCS
      src/darwin/DarwinDataSource.mm
  )
endif ()

# ==================================================================================================
# Include and target definitions
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})
include_directories(src)

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
target_link_libraries(${TARGET} PRIVATE minilog)
if (ANDROID)
  target_link_libraries(${TARGET} PRIVATE jni)
endif ()
if (WIN32)
  target_link_libraries(${TARGET} PRIVATE winhttp)
endif ()
if (APPLE)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(${TARGET} PRIVATE ${FOUNDATION_FRAMEWORK})
  set_source_files_properties(
      src/darwin/DarwinDataSource.mm
      src/darwin/DarwinHttpDataSource.mm
      PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(${TARGET} PROPERTIES
      OBJCXX_STANDARD 20
      OBJCXX_STANDARD_REQUIRED ON
  )
endif ()
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# ==================================================================================================
# Compiler flags
# ==================================================================================================
if (MSVC)
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:/fp:fast>)
else ()
  target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:-ffast-math -fno-finite-math-only>)
  target_compile_options(${TARGET} PRIVATE -Wno-deprecated-register)
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Tests
# ==================================================================================================
if (CORAL_BUILD_TESTS)

  enable_testing()
  add_subdirectory(tests)

endif ()
