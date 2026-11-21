# ==================================================================================================
# CoralMedia dependencies
# ==================================================================================================
# Adds every target the modules in this repository link, so that the framework can be configured on its
# own:
#
#   # every submodule is declared `update = none`, so the standalone checkout overrides the strategy
#   # for the ones it needs; `--recursive` also brings in Minilog and zlib's own sources
#   git -c submodule.third_party/libz.update=checkout \
#       -c submodule.third_party/libyuv.update=checkout \
#       -c submodule.third_party/libgtest.update=checkout \
#       -c submodule.external/CoralCore.update=checkout \
#       submodule update --init --recursive
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# The application root adds its own dependency graph before CoralMedia, and every `add_subdirectory`
# here is guarded by `NOT TARGET`, so including this file in that tree is a no-op. That is what keeps
# the two layouts apart:
#
#   standalone                         this repository is the top-level project
#     external/CoralCore               the foundation framework: `utils` and `minilog`
#     third_party/libz                 zlib, for the compressed subtitle and PNG paths in extractor
#     third_party/libyuv               colour conversion, for the FFmpeg codecs in ffcodec
#     third_party/libgtest             the test suites
#     third_party/ffmpeg               the prebuilt FFmpeg package (checked-in files), with FFmpeg on
#
#   as frameworks/CoralMedia           this repository is a submodule of the application
#     frameworks/CoralCore             a sibling, added by the application before CoralMedia
#     third_party/libz                 the application's third_party/libz
#     third_party/libyuv               the application's third_party/libyuv
#     third_party/libgtest             the application's third_party/libgtest
#
# Every submodule of this repository carries `update = none` in .gitmodules, and that is what makes the
# submodule layout work: `git submodule update --init --recursive` in the application downloads none of
# them, because the application already provides CoralCore, libz, libyuv and libgtest as siblings, and
# the copies here would never even be configured. `update = none` is exactly the instruction to skip
# the submodule, so neither a plain update nor `--force` fetches it. A standalone checkout overrides
# the strategy per submodule with the `-c` options on the command above.
#
# Nested submodules are `update = none` for the same reason: CoralCore declares its libgtest that way,
# so the GoogleTest copy under external/CoralCore stays uninitialized (this repository supplies
# GoogleTest), while Minilog is fetched by the recursive update.

# `third_party/ffmpeg` is not a submodule. The prebuilt package is checked into this repository as
# headers plus per-platform libraries, so no configure ever needs the FFmpeg sources, and updating it
# means replacing those files in place. It is only used with -DCORAL_SUPPORTS_FFMPEG=ON.
#
# `CORAL_SHARED_THIRD_PARTY_DIR` remains an explicit override for a tree that keeps libz, libyuv and
# libgtest outside both layouts; the application's `third_party/` is the default when it is a sibling.

include_guard(GLOBAL)

get_filename_component(CORALMEDIA_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ==================================================================================================
# Feature switches
# ==================================================================================================
# Declared here as well as in the root CMakeLists so that a project which includes this file before
# adding the framework configures the same feature set. `option()` keeps a value the caller supplied.
option(CORAL_SUPPORTS_FFMPEG "Build the FFmpeg-backed ffcodec module" OFF)

# ==================================================================================================
# Platform switches
# ==================================================================================================
# The application root sets these; a standalone configure has to set the same ones, because the module
# source lists are selected with them. Setting them here means the framework root and any client
# project agree without either repeating the rule.
if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT WASM)
  set(LINUX TRUE)
else ()
  set(LINUX FALSE)
endif ()

if (NOT DEFINED IS_HOST_PLATFORM)
  set(IS_HOST_PLATFORM TRUE)
endif ()

# ==================================================================================================
# The shared third-party tree
# ==================================================================================================
# libz, libyuv and libgtest are also carried as git submodules of this repository, so a standalone
# configure needs nothing beyond fetching them; the bootstrap command in the header of this file
# overrides their `update = none` strategy for exactly that.
#
# A tree that keeps them outside this repository names another directory here. When CoralMedia is added
# as a subproject the application has already added these targets, so the guards below skip every
# bundled copy and the application's versions are the ones linked.
if (NOT DEFINED CORAL_SHARED_THIRD_PARTY_DIR
    AND EXISTS "${CORALMEDIA_REPO_DIR}/../../third_party/libgtest/coral/CMakeLists.txt")
  set(CORAL_SHARED_THIRD_PARTY_DIR "${CORALMEDIA_REPO_DIR}/../../third_party")
endif ()

# ==================================================================================================
# CoralCore
# ==================================================================================================
# `utils` and `minilog` come from the foundation framework. A standalone checkout carries it as the
# `external/CoralCore` submodule; a workgroup checkout keeps it beside this repository. The directory
# is resolved here but added after GoogleTest, so that this repository's bundled gtest wins over
# CoralCore's copy (CoralCore's CMakeLists is guarded by `NOT TARGET gtest` too).
set(_coral_core_dir "")
if (NOT TARGET utils)
  if (DEFINED CORAL_CORE_DIR AND EXISTS "${CORAL_CORE_DIR}/CMakeLists.txt")
    set(_coral_core_dir "${CORAL_CORE_DIR}")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/external/CoralCore/CMakeLists.txt")
    set(_coral_core_dir "${CORALMEDIA_REPO_DIR}/external/CoralCore")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/../CoralCore/CMakeLists.txt")
    set(_coral_core_dir "${CORALMEDIA_REPO_DIR}/../CoralCore")
  else ()
    message(FATAL_ERROR
        "CoralMedia needs CoralCore for its `utils` and `minilog` targets. Initialize the submodule "
        "with\n"
        "  git -c submodule.external/CoralCore.update=checkout submodule update --init --recursive "
        "external/CoralCore\n"
        "keep a sibling ../CoralCore checkout, pass -DCORAL_CORE_DIR=<path>, or add the CoralCore "
        "targets before CoralMedia.")
  endif ()
endif ()

# ==================================================================================================
# GoogleTest
# ==================================================================================================
# Only the test suites name it. This repository's third_party/libgtest is the default, so the framework
# does not depend on CoralCore's copy of it. An explicit CORAL_GTEST_DIR or a shared tree wins, and
# CoralCore's copy is the last fallback for a checkout whose bundled submodule is missing.
if (CORAL_BUILD_TESTS AND NOT TARGET gtest)
  if (DEFINED CORAL_GTEST_DIR AND EXISTS "${CORAL_GTEST_DIR}/CMakeLists.txt")
    add_subdirectory("${CORAL_GTEST_DIR}" "${CMAKE_BINARY_DIR}/gtest")
  elseif (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/third_party/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${CORALMEDIA_REPO_DIR}/third_party/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  elseif (DEFINED _coral_core_dir AND EXISTS "${_coral_core_dir}/third_party/libgtest/coral/CMakeLists.txt")
    add_subdirectory("${_coral_core_dir}/third_party/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
  endif ()

  if (NOT TARGET gtest)
    message(FATAL_ERROR
        "CoralMedia tests need GoogleTest. Fetch the submodule with\n"
        "  git -c submodule.third_party/libgtest.update=checkout submodule update --init --recursive "
        "third_party/libgtest\n"
        "pass -DCORAL_GTEST_DIR=<path>, point -DCORAL_SHARED_THIRD_PARTY_DIR at a tree with libgtest, "
        "or configure with -DCORAL_BUILD_TESTS=OFF.")
  endif ()
endif ()

# ==================================================================================================
# Adding CoralCore
# ==================================================================================================
if (NOT TARGET utils)
  add_subdirectory("${_coral_core_dir}" "${CMAKE_BINARY_DIR}/coral-core")
endif ()

# ==================================================================================================
# libz
# ==================================================================================================
# zlib decompresses the embedded bitmap and font payloads of the subtitle formats the extractor reads.
# It comes from this repository's bundled submodule, or from an explicit shared tree.
if (NOT TARGET z)
  if (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral" "${CMAKE_BINARY_DIR}/libz")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/third_party/libz/coral/CMakeLists.txt")
    add_subdirectory("${CORALMEDIA_REPO_DIR}/third_party/libz/coral" "${CMAKE_BINARY_DIR}/libz")
  endif ()

  if (NOT TARGET z)
    message(FATAL_ERROR
        "CoralMedia needs the zlib wrapper (target `z`) for its subtitle and PNG data. Fetch the "
        "submodule with\n"
        "  git -c submodule.third_party/libz.update=checkout submodule update --init --recursive "
        "third_party/libz\n"
        "pass -DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>, or add the zlib target before "
        "CoralMedia.")
  endif ()
endif ()

# ==================================================================================================
# libyuv
# ==================================================================================================
# Only ffcodec needs it: the FFmpeg codecs convert between the planar formats FFmpeg produces and the
# NV12/I420 buffers the rest of the framework uses. The application has added `yuv` before CoralMedia
# when FFmpeg is on, so the guarded `add_subdirectory` below is a no-op there.
if (CORAL_SUPPORTS_FFMPEG AND NOT TARGET yuv)
  if (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv" "${CMAKE_BINARY_DIR}/libyuv")
    set(_coral_yuv_include "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/include")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/third_party/libyuv/CMakeLists.txt")
    add_subdirectory("${CORALMEDIA_REPO_DIR}/third_party/libyuv" "${CMAKE_BINARY_DIR}/libyuv")
    set(_coral_yuv_include "${CORALMEDIA_REPO_DIR}/third_party/libyuv/include")
  endif ()

  if (NOT TARGET yuv)
    message(FATAL_ERROR
        "CORAL_SUPPORTS_FFMPEG needs libyuv (target `yuv`). Fetch the submodule with\n"
        "  git -c submodule.third_party/libyuv.update=checkout submodule update --init third_party/libyuv\n"
        "pass -DCORAL_SHARED_THIRD_PARTY_DIR=<application third_party>, or configure with "
        "-DCORAL_SUPPORTS_FFMPEG=OFF.")
  endif ()
endif ()

# libyuv's own CMake lists its include directory at directory scope, which reaches the `yuv` target but
# not the targets that link it. ffcodec includes <libyuv.h> in its own translation units, so the
# directory has to be part of the target's usage requirements.
if (CORAL_SUPPORTS_FFMPEG AND TARGET yuv)
  if (EXISTS "${CORALMEDIA_REPO_DIR}/third_party/libyuv/include")
    target_include_directories(yuv PUBLIC "${CORALMEDIA_REPO_DIR}/third_party/libyuv/include")
  elseif (EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/include")
    target_include_directories(yuv PUBLIC "${CORAL_SHARED_THIRD_PARTY_DIR}/libyuv/include")
  endif ()
endif ()

# ==================================================================================================
# FFmpeg
# ==================================================================================================
# The prebuilt package owns the headers, the imported target and the per-platform libraries; there is
# nothing to compile. It is checked into this repository, so there is no submodule to initialize. The
# application includes it before CoralMedia, which the `NOT TARGET` guard recognises.
if (CORAL_SUPPORTS_FFMPEG AND NOT TARGET FFmpeg::ffmpeg)
  if (DEFINED CORAL_FFMPEG_DIR AND EXISTS "${CORAL_FFMPEG_DIR}/cmake/FFmpegConfig.cmake")
    include("${CORAL_FFMPEG_DIR}/cmake/FFmpegConfig.cmake")
  elseif (EXISTS "${CORALMEDIA_REPO_DIR}/third_party/ffmpeg/cmake/FFmpegConfig.cmake")
    include("${CORALMEDIA_REPO_DIR}/third_party/ffmpeg/cmake/FFmpegConfig.cmake")
  endif ()

  if (NOT TARGET FFmpeg::ffmpeg)
    message(FATAL_ERROR
        "CORAL_SUPPORTS_FFMPEG needs the prebuilt FFmpeg package: the headers, the per-platform "
        "libraries and cmake/FFmpegConfig.cmake under third_party/ffmpeg. Restore that directory, pass "
        "-DCORAL_FFMPEG_DIR=<path>, or configure with -DCORAL_SUPPORTS_FFMPEG=OFF.")
  endif ()
endif ()
