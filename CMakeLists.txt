# ==================================================================================================
# CoralMedia
# ==================================================================================================
# The media framework: the `media` data model plus the datasource, extractor, codec, muxer, media-registry
# and (optionally) ffcodec modules. The modules live under modules/; this root resolves the framework's
# dependencies and gives a standalone checkout the same entry point as the application.
#
# Two ways this repository is built, both through this file:
#
#   standalone, as its own project
#     git -c submodule.third_party/libz.update=checkout \
#         -c submodule.third_party/libyuv.update=checkout \
#         -c submodule.third_party/libgtest.update=checkout \
#         -c submodule.external/CoralCore.update=checkout \
#         submodule update --init --recursive
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#     ctest --test-dir build --output-on-failure
#
#   as frameworks/CoralMedia inside the application
#     The application's cmake/Dependencies.cmake adds CoralCore, libz, libyuv and libgtest before
#     CoralMedia, so cmake/CoralMediaDependencies.cmake finds every target already defined and adds
#     nothing: the sibling frameworks/CoralCore is the one that is built, and no submodule of this
#     repository is downloaded into the application tree. Every entry in .gitmodules carries
#     `update = none` for exactly that reason, which is also why the standalone bootstrap above
#     overrides the strategy per submodule.
#
# FFmpeg is optional and off by default; -DCORAL_SUPPORTS_FFMPEG=ON adds the ffcodec module, which
# needs the prebuilt package under third_party/ffmpeg.

cmake_minimum_required(VERSION 3.19)
project(CoralMedia LANGUAGES CXX)

# The Apple platform layer of the codec module is Objective-C++: the AVFoundation extractor and
# muxer are written against the Objective-C classes of the framework. The language has to be
# enabled before the module is added, otherwise CMake has no rule for its `.mm` sources.
if (APPLE)
  enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CoralBuild.cmake)
coral_apply_global_build_settings()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CORALMEDIA_TOP_LEVEL ON)
else ()
  set(CORALMEDIA_TOP_LEVEL OFF)
endif ()

if (CORALMEDIA_TOP_LEVEL)
  if (NOT DEFINED IS_HOST_PLATFORM)
    set(IS_HOST_PLATFORM TRUE)
  endif ()
  # The public switch: a caller's -DCORAL_BUILD_TESTS=OFF must win over the standalone default of ON.
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS ON CACHE BOOL "Build the CoralMedia tests")
  endif ()
else ()
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralMedia tests")
  endif ()
endif ()
# The module-level name mirrors the public one, and is forced so that changing the public switch in an
# existing build tree takes effect on the next configure rather than being masked by the old cache entry.
set(CORALMEDIA_BUILD_TESTS ${CORAL_BUILD_TESTS} CACHE BOOL "Build the CoralMedia tests" FORCE)
set(CORAL_BUILD_TESTS ${CORALMEDIA_BUILD_TESTS})
if (CORAL_BUILD_TESTS)
  enable_testing()
endif ()

# ==================================================================================================
# Feature switches
# ==================================================================================================
# The application root sets these before adding the framework; the defaults here match it so a standalone
# checkout works without a cache file.
option(CORAL_SUPPORTS_FFMPEG "Build the FFmpeg-backed ffcodec module" OFF)

# ==================================================================================================
# Test fixtures
# ==================================================================================================
# A framework checkout keeps its fixtures in testdata/; the application keeps them in assets/ at its
# root, which is two directories above frameworks/CoralMedia. Neither layout is required when a suite
# passes its fixtures as arguments, so an undefined value is left undefined.
if (NOT DEFINED CORAL_TEST_DATA_DIR)
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    set(CORAL_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/testdata")
  elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../assets")
    set(CORAL_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../assets")
  endif ()
endif ()

# ==================================================================================================
# Dependencies
# ==================================================================================================
# Resolves CoralCore and every third-party target the modules link, for a configure that starts here.
# The application root resolves them itself; the manifest is guarded by `NOT TARGET`, so including it
# there adds nothing.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/CoralMediaDependencies.cmake")

if (NOT TARGET utils OR NOT TARGET z)
  message(FATAL_ERROR
      "CoralMedia dependency targets are missing. Configure this repository standalone so that "
      "cmake/CoralMediaDependencies.cmake runs, or add CoralCore and the application's third-party "
      "targets before CoralMedia.")
endif ()

# The media-only third-party libraries now live in this repository. The application's shared
# third-party tree (libyuv/libz/libgtest) is outside it; keep EXTERNAL pointing here so the modules
# read this framework's own tree.
set(EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ==================================================================================================
# Modules
# ==================================================================================================
# CoralMedia is data-only: it must not depend on CoralGraphics. The media data model stays usable by a
# media-only consumer, which is why the modules are added one by one rather than from a list.
add_subdirectory(modules/media)
add_subdirectory(modules/datasource)
add_subdirectory(modules/extractor)
add_subdirectory(modules/codec)
add_subdirectory(modules/muxer)

if (CORAL_SUPPORTS_FFMPEG)
  add_subdirectory(modules/ffcodec)
endif ()

add_subdirectory(modules/media-registry)

# ==================================================================================================
# Examples
# ==================================================================================================
# The validation program: it encodes synthetic audio and video, muxes them, demuxes the file and
# decodes the tracks, so a standalone configure proves the four stages work in the tree that was just
# built. The project is also configurable on its own (`cmake -S examples ...`); an application that
# adds this framework as a subproject does not get example executables.
if (CORALMEDIA_TOP_LEVEL)
  add_subdirectory(examples)
endif ()
