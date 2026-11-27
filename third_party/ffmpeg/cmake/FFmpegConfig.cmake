# ==================================================================================================
# Coral FFmpeg package
# ==================================================================================================
# Consumes the package produced by CoralFrameworks/FfmpegBuilder. Each platform/arch directory owns
# its headers and libraries, because FFmpeg generates architecture-specific headers (avconfig.h) and
# the ABI of the shared libraries differs between platforms:
#
#   windows/<arch>/include, windows/<arch>/*.dll, windows/<arch>/*.dll.a
#   linux/<arch>/include,   linux/<arch>/libav*.so*
#   android/<abi>/include,  android/<abi>/libffmpeg.so or libav*.so
#   apple/<arch>/include,   apple/<arch>/libav*.dylib or libav*.a
#
# The layout is intentionally a drop-in directory: replacing the shared libraries with an
# ABI-compatible build (same FFmpeg major version) does not require rebuilding the player. The
# application must ship the DLLs/SOs next to its executable (Windows) or on LD_LIBRARY_PATH (Linux).

if (TARGET FFmpeg::ffmpeg)
  return()
endif ()

get_filename_component(_ffmpeg_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if (WIN32)
  set(_ffmpeg_platform "windows")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
    set(_ffmpeg_arch "x64")
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64)$")
    set(_ffmpeg_arch "arm64")
  else ()
    set(_ffmpeg_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif ()
elseif (ANDROID)
  set(_ffmpeg_platform "android")
  set(_ffmpeg_arch "${ANDROID_ABI}")
elseif (APPLE)
  set(_ffmpeg_platform "apple")
  if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_ffmpeg_arch "arm64")
  else ()
    set(_ffmpeg_arch "x86_64")
  endif ()
elseif (UNIX)
  set(_ffmpeg_platform "linux")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64)$")
    set(_ffmpeg_arch "aarch64")
  else ()
    set(_ffmpeg_arch "x86_64")
  endif ()
else ()
  message(FATAL_ERROR "FFmpegConfig: unsupported platform")
endif ()

set(_ffmpeg_dir "${_ffmpeg_root}/${_ffmpeg_platform}/${_ffmpeg_arch}")
set(_ffmpeg_include "${_ffmpeg_dir}/include")

if (NOT EXISTS "${_ffmpeg_include}")
  message(FATAL_ERROR "FFmpegConfig: no headers under ${_ffmpeg_include}")
endif ()

add_library(FFmpeg::ffmpeg INTERFACE IMPORTED)
set_target_properties(FFmpeg::ffmpeg PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_ffmpeg_include}"
)

if (WIN32)
  file(GLOB _ffmpeg_libs
    "${_ffmpeg_dir}/*.dll.a"
    "${_ffmpeg_dir}/*.lib")
  if (NOT _ffmpeg_libs)
    message(FATAL_ERROR "FFmpegConfig: no import libraries under ${_ffmpeg_dir}")
  endif ()
  # The DLLs are resolved at runtime; expose their directory so an application can copy them next
  # to its executable (the player does this for the demo and the tests).
  set(FFMPEG_DLL_DIR "${_ffmpeg_dir}" CACHE PATH "Directory with the FFmpeg runtime DLLs")
  set_property(TARGET FFmpeg::ffmpeg PROPERTY
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
elseif (ANDROID)
  file(GLOB _ffmpeg_libs
    "${_ffmpeg_dir}/libffmpeg.so"
    "${_ffmpeg_dir}/libav*.so")
  if (NOT _ffmpeg_libs)
    message(FATAL_ERROR "FFmpegConfig: no shared libraries under ${_ffmpeg_dir}")
  endif ()
  set_property(TARGET FFmpeg::ffmpeg PROPERTY
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
elseif (APPLE)
  file(GLOB _ffmpeg_libs
    "${_ffmpeg_dir}/libav*.dylib"
    "${_ffmpeg_dir}/libav*.a")
  if (NOT _ffmpeg_libs)
    message(FATAL_ERROR "FFmpegConfig: no libraries under ${_ffmpeg_dir}")
  endif ()
  set_property(TARGET FFmpeg::ffmpeg PROPERTY
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
else ()
  file(GLOB _ffmpeg_libs "${_ffmpeg_dir}/libav*.so" "${_ffmpeg_dir}/libsw*.so")
  if (NOT _ffmpeg_libs)
    message(FATAL_ERROR "FFmpegConfig: no shared libraries under ${_ffmpeg_dir}")
  endif ()
  set(FFMPEG_LIB_DIR "${_ffmpeg_dir}" CACHE PATH "Directory with the FFmpeg shared libraries")
  set_property(TARGET FFmpeg::ffmpeg PROPERTY
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
endif ()

# The package records its version and build flags; a consumer only needs to know the major version
# of the ABI it links against (a replacement package may use the same one).
if (EXISTS "${_ffmpeg_root}/BUILD_INFO.txt")
  file(STRINGS "${_ffmpeg_root}/BUILD_INFO.txt" _ffmpeg_version_line REGEX "^FFmpeg version:")
  if (_ffmpeg_version_line)
    string(REGEX REPLACE "^FFmpeg version:[ \t]*" "" FFMPEG_VERSION "${_ffmpeg_version_line}")
    set(FFMPEG_VERSION "${FFMPEG_VERSION}" CACHE STRING "FFmpeg package version")
  endif ()
endif ()

unset(_ffmpeg_root)
unset(_ffmpeg_platform)
unset(_ffmpeg_arch)
unset(_ffmpeg_dir)
unset(_ffmpeg_include)
unset(_ffmpeg_libs)
