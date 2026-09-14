cmake_minimum_required(VERSION 3.16)

set(CPP_STD 17)
set(CMAKE_CXX_STANDARD ${CPP_STD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(pcm_coder)

option(ENABLE_PLAYER "Build SDL2/PortAudio playback support" ON)
option(ENABLE_LEGACY_RPI "Use legacy DispmanX and build rpi-fb-shifter (not for KMS)" OFF)

set(MODULES_DIR ${CMAKE_SOURCE_DIR}/cmake)

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${MODULES_DIR})

add_subdirectory(libs)
add_subdirectory(src)

