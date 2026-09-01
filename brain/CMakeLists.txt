# Brain build. Nothing links against the simulator: the only thing this library
# needs is the SDK include directory.
#
#   cmake -S brain -B brain/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build brain/build
#
# Produces brain/build/brain.so (or brain.dll on Windows).

cmake_minimum_required(VERSION 3.16)
project(swarm_brain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BRAIN_SOURCES
    src/brain.cpp
    src/belief.cpp
    src/flight.cpp
    src/policy.cpp
)

add_library(brain SHARED ${BRAIN_SOURCES})

target_include_directories(brain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# No "lib" prefix, and hide everything except the one exported factory. Hiding
# matters beyond tidiness: fewer exported symbols means the loader cannot bind
# something of ours to something of theirs by accident.
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
# error to load time. This is the check that proves the library is standalone.
if(NOT WIN32 AND NOT APPLE)
    target_link_options(brain PRIVATE -Wl,--no-undefined)
endif()

# ---------------------------------------------------------------------------
# Tests. Plain executables that return non-zero on failure -- no framework to
# vendor, and `ctest` or `make test` drives them either way.
# ---------------------------------------------------------------------------

enable_testing()

add_executable(test_protocol tests/test_protocol.cpp)
target_include_directories(test_protocol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_policy tests/test_policy.cpp src/policy.cpp src/belief.cpp src/flight.cpp)
target_include_directories(test_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME policy COMMAND test_policy)

add_executable(test_belief tests/test_belief.cpp src/belief.cpp)
target_include_directories(test_belief PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME belief COMMAND test_belief)
