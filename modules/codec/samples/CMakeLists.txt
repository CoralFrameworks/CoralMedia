# ==================================================================================================
# Samples
#
# Each sample is a standalone executable named codec_<Thing>.
# ==================================================================================================

set(SAMPLE_NAMES
    codec_media_info
    codec_audio_transcode
    codec_video_pipeline
    codec_muxer
)

foreach (SAMPLE_NAME ${SAMPLE_NAMES})
  add_executable(${SAMPLE_NAME} ${SAMPLE_NAME}.cpp)
  target_link_libraries(${SAMPLE_NAME} PRIVATE codec)
  target_link_libraries(${SAMPLE_NAME} PRIVATE minilog)
  if (MSVC)
    # the windows media foundation headers are used by the samples.
    target_compile_definitions(${SAMPLE_NAME} PRIVATE NOMINMAX)
  endif ()
  set_target_properties(${SAMPLE_NAME} PROPERTIES FOLDER Samples)
endforeach ()
