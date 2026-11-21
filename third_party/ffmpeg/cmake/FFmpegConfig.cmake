# ==================================================================================================
# Coral FFmpeg prebuilt package
# ==================================================================================================
# This package contains headers plus prebuilt FFmpeg libraries. The library layout is:
#
#   include/                 FFmpeg headers and config.h
#   android/<abi>/            libffmpeg.so for Android
#   windows/<arch>/          *.lib (import/static) and *.dll
#   linux/<arch>/            libav*.so or libav*.a
#
# The package does not compile FFmpeg itself. BUILD_INFO.txt records the upstream version and the
# configure/compiler settings used to produce the libraries.

if (TARGET FFmpeg::ffmpeg)
  return()
endif ()

get_filename_component(_ffmpeg_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_ffmpeg_include "${_ffmpeg_root}/include")

add_library(FFmpeg::ffmpeg INTERFACE IMPORTED)
set_target_properties(FFmpeg::ffmpeg PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_ffmpeg_include}"
)

if (ANDROID)
  if (NOT ANDROID_ABI)
    message(FATAL_ERROR "FFmpegConfig: ANDROID_ABI is not set")
  endif ()
  set(_ffmpeg_library "${_ffmpeg_root}/android/${ANDROID_ABI}/libffmpeg.so")
  if (NOT EXISTS "${_ffmpeg_library}")
    message(FATAL_ERROR "FFmpegConfig: missing Android library ${_ffmpeg_library}")
  endif ()
  set_property(TARGET FFmpeg::ffmpeg PROPERTY
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_library}")
elseif (WIN32)
  file(GLOB _ffmpeg_libs
    "${_ffmpeg_root}/windows/${CMAKE_SYSTEM_PROCESSOR}/*.lib")
  if (_ffmpeg_libs)
    set_property(TARGET FFmpeg::ffmpeg PROPERTY
      INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
  else ()
    message(WARNING
        "FFmpegConfig: no Windows libraries under ${_ffmpeg_root}/windows/${CMAKE_SYSTEM_PROCESSOR}; "
        "headers are available but FFmpeg consumers cannot link")
  endif ()
elseif (UNIX)
  file(GLOB _ffmpeg_libs
    "${_ffmpeg_root}/linux/${CMAKE_SYSTEM_PROCESSOR}/libav*.so"
    "${_ffmpeg_root}/linux/${CMAKE_SYSTEM_PROCESSOR}/libav*.a")
  if (_ffmpeg_libs)
    set_property(TARGET FFmpeg::ffmpeg PROPERTY
      INTERFACE_LINK_LIBRARIES "${_ffmpeg_libs}")
  else ()
    message(WARNING
        "FFmpegConfig: no Linux libraries under ${_ffmpeg_root}/linux/${CMAKE_SYSTEM_PROCESSOR}; "
        "headers are available but FFmpeg consumers cannot link")
  endif ()
endif ()
