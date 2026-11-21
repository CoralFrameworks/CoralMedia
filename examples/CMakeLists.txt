# ==================================================================================================
# CoralMedia examples
# ==================================================================================================
# The framework's standalone validation program. It drives the public API of the modules and reports
# whether demux, decode, encode and mux work in the tree that was just built:
#
#   standalone, as its own project
#     cmake -S examples -B build/examples -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build/examples
#     ./build/examples/validate/coralmedia_validate
#
#   added by the framework root
#     A top-level configure of the repository adds this directory (see the root CMakeLists), so the
#     standalone build and `ctest` cover the examples without a second configure.
#
# The validation program needs no fixture: it encodes synthetic audio and video, muxes the result,
# demuxes and decodes it again. Pass `--input <file>` to also run the demux and decode stages on a
# real media file, which is what a checkout with testdata/ should do.

set(CORALMEDIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone. Resolve the dependencies and add the framework the same way the application root does:
  # the dependency graph first, then the framework, then the examples.
  cmake_minimum_required(VERSION 3.19)
  project(CoralMediaExamples LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  include("${CORALMEDIA_ROOT}/cmake/CoralBuild.cmake")
  coral_apply_global_build_settings()

  set(IS_HOST_PLATFORM TRUE)

  # The examples link the framework's libraries; they do not build or run its unit suites, which is why
  # this project does not need GoogleTest even when the shared third-party tree carries it.
  set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralMedia tests" FORCE)

  include("${CORALMEDIA_ROOT}/cmake/CoralMediaDependencies.cmake")
  add_subdirectory("${CORALMEDIA_ROOT}" "${CMAKE_BINARY_DIR}/coral-media")
endif ()

enable_testing()

add_subdirectory(validate)
