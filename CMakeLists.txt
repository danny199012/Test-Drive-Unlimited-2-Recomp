# tdu2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tdu2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TDU2_SOURCES
    src/main.cpp
    src/tdu2_menu.cpp
    src/tdu2_profile.cpp
)

if(WIN32)
    add_executable(tdu2 WIN32 ${TDU2_SOURCES})
else()
    add_executable(tdu2 ${TDU2_SOURCES})
endif()

# Stage the Xenos GPU emulation plugin next to the exe. rexglue picks the
# per-configuration variant (rexgpu-xenosd.dll for Debug, rexgpu-xenos.dll
# for Release). Without this the game has no GPU emulation at all.
rexglue_setup_target(tdu2 GPU_PLUGINS xenos)

# The SDK headers use clang builtins, so the generated recomp code cannot be
# compiled by plain MSVC cl. Silence the UCRT deprecation noise our own
# headers would otherwise produce under clang.
if(WIN32)
    target_compile_definitions(tdu2 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Stage the cvar config next to the exe. The runtime reads tdu2.toml from the
# exe's directory at startup, and the profile settings are rewritten there on
# shutdown, so the file has to live there rather than in the source tree.
#
# configure_file(COPYONLY) overwrites unconditionally, which would discard both
# hand edits and the profile saved on the last run. Copy only when missing so
# the template acts as a first-run default; deleting out/build/<preset> resets.
set(TDU2_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/tdu2.toml")
if(NOT EXISTS "${TDU2_CONFIG}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/config/tdu2.toml.template"
        "${TDU2_CONFIG}"
        COPYONLY)
endif()
