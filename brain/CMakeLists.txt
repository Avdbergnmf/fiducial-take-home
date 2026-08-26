cmake_minimum_required(VERSION 3.16)
project(swarm_brain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(BRAIN_SOURCES
    src/brain.cpp
)

add_library(brain SHARED ${BRAIN_SOURCES})

target_include_directories(brain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
)

set_target_properties(brain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "brain"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
    target_compile_options(brain PRIVATE /W4)
else()
    target_compile_options(brain PRIVATE -Wall -Wextra)
endif()

# Fail the build on an unresolved symbol instead of deferring it to dlopen time.
if(NOT WIN32 AND NOT APPLE)
    target_link_options(brain PRIVATE -Wl,--no-undefined)
endif()
