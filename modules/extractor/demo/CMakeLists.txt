cmake_minimum_required(VERSION 3.19)
set(TARGET extractor_demo)

# ==================================================================================================
# Demo
# ==================================================================================================

add_executable(${TARGET}
    src/main.cpp
)

target_link_libraries(${TARGET} PRIVATE datasource extractor media utils)

set_target_properties(${TARGET} PROPERTIES FOLDER Samples)
