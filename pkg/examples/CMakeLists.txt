# Build template for a challenge submission.
#
# Copy this file next to your own sources, change BRAIN_SOURCES, and build:
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# The result is brain.dll (Windows) or brain.so (Linux/macOS) in build/.
# Point the simulator at it with:
#
#   swarm_sim --scenario s1 --brain path/to/brain.dll
#
# Note that nothing here links against the simulator. The only thing your
# library needs is the SDK include directory.

cmake_minimum_required(VERSION 3.16)
project(swarm_brain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BRAIN_SOURCES
    hover_relay_brain.cpp
)

add_library(brain SHARED ${BRAIN_SOURCES})

target_include_directories(brain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include)

# No "lib" prefix, and hide everything except the one exported factory.
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

# Fail the build if any symbol is left unresolved, rather than deferring the
# error to load time. This is the check that proves your library is genuinely
# standalone.
if(NOT WIN32 AND NOT APPLE)
    target_link_options(brain PRIVATE -Wl,--no-undefined)
endif()
