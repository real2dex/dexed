cmake_minimum_required(VERSION 3.16)
project(DexedCli VERSION 1.0.1)

# Headless command line renderer built from the Dexed synth engine.
# There is no GUI and no plugin format here; see the `real2dex` branch for those.

#Compile commands, useful for some IDEs like VS-Code
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(NOT BUILD_ID)
    add_definitions("-DDEXED_ID=DEVBUILD")
else()
    add_definitions("-DDEXED_ID=${BUILD_ID}")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum OS X deployment version" FORCE)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE INTERNAL "")
endif()

#static linking in Windows
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# include JUCE *AFTER* the MSVC runtime and so on is set up
set(DEXED_JUCE_PATH "${CMAKE_SOURCE_DIR}/libs/JUCE" CACHE STRING "Path to JUCE library source tree")
add_subdirectory(${DEXED_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)

#Adds all the module sources so they appear correctly in the IDE
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Enable Module Source Groups" ON)

option(JUCE_BUILD_EXTRAS "Build JUCE Extras" OFF)
option(JUCE_BUILD_EXAMPLES "Build JUCE Examples" OFF)

add_subdirectory(libs)
add_subdirectory(assets)
add_subdirectory(Source)

if (APPLE)
target_compile_options(dexed-cli PUBLIC
        #-Werror
        )
elseif(UNIX AND NOT APPLE)
target_compile_options(dexed-cli PUBLIC
        -Wno-deprecated-declarations
        -Wno-unused-value
        )
endif()
