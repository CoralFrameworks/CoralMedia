cmake_minimum_required(VERSION 3.19)
set(TARGET media)
set(PUBLIC_HDR_DIR include)

# ==================================================================================================
# Sources and headers
# ==================================================================================================

set(PUBLIC_HDRS
    include/media/BlockBuffer.h
    include/media/Buffer.h
    include/media/ColorInfo.h
    include/media/Configuration.h
    include/media/CryptoInfo.h
    include/media/Definitions.h
    include/media/Format.h
    include/media/Metadata.h
    include/media/MetadataEntries.h
    include/media/PacketBuffer.h
    include/media/PixelBuffer.h
    include/media/PixelBufferHandle.h
    include/media/PixelBufferD3D11.h
    include/media/PixelBufferAndroid.h
    include/media/SyncToken.h
    include/media/SyncTokenD3D11.h
    include/media/SurfaceFrameSource.h
    include/media/SampleBuffer.h
    include/media/Time.h
    include/media/TimeRange.h
    include/media/Types.h
    include/media/Utilities.h
)

set(SRCS
    src/Format.cpp
    src/FrameBuffer.cpp
    src/ostream.cpp
    src/PacketBuffer.cpp
    src/PathUtils.h
    src/PixelBuffer.cpp
    src/PixelBufferHandle.cpp
    src/SampleBuffer.cpp
    src/TimeRange.cpp
    src/Types.cpp
    src/Utilities.cpp
)

if(WIN32)
    list(APPEND SRCS src/PixelBufferD3D11.cpp src/SyncTokenD3D11.cpp)
endif()

if(ANDROID)
    list(APPEND SRCS src/PixelBufferAndroid.cpp)
endif()

# ==================================================================================================
# Includes & target definition
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})


add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_compile_options(${TARGET} PRIVATE ${OPTIMIZATION_FLAGS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)


# ==================================================================================================
# Dependencies
# ==================================================================================================
target_link_libraries(${TARGET} PUBLIC utils)
if(WIN32)
    target_link_libraries(${TARGET} PUBLIC d3d11 dxgi)
endif()
if(ANDROID)
    target_link_libraries(${TARGET} PUBLIC android)
endif()

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Test
# ==================================================================================================

