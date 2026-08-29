cmake_minimum_required(VERSION 3.16)
project(swarm_brain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A glob is deliberate: sources get pasted in and renamed all week and I do not
# want to edit this file every time. CONFIGURE_DEPENDS makes the build re-glob
# when the directory changes. Replace this with an explicit list before
# submission -- a glob is a reasonable dev-loop shortcut, not a reviewable one.
file(GLOB BRAIN_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

if(NOT BRAIN_SOURCES)
    message(FATAL_ERROR
        "No .cpp files in brain/src/. Paste the brain sources in there, then "
        "re-run the configure step. (Configuring an empty SHARED library fails "
        "later and more confusingly than this message does.)")
endif()

add_library(brain SHARED ${BRAIN_SOURCES})

target_include_directories(brain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pkg/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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
