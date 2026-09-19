# ravel — deterministic simulation testing for C++ distributed systems.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(ravel LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone build vs. consumed as a subdirectory.
set(RAVEL_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RAVEL_STANDALONE ON)
endif()

option(RAVEL_SHARED         "Also build a shared library"  OFF)
option(RAVEL_BUILD_TESTS    "Build the test suite"         ${RAVEL_STANDALONE})
option(RAVEL_BUILD_BENCH    "Build the perf bench"         ${RAVEL_STANDALONE})
option(RAVEL_BUILD_EXAMPLES "Build the example programs"   ${RAVEL_STANDALONE})

set(RAVEL_SOURCES
  src/c_api.cpp
  src/network.cpp
  src/rng.cpp
  src/scheduler.cpp
  src/simulation.cpp
  src/trace.cpp
  src/version.cpp
)

# Declared once so the static and shared variants can never drift apart.
function(ravel_configure_library target)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(ravel STATIC ${RAVEL_SOURCES})
add_library(ravel::ravel ALIAS ravel)
ravel_configure_library(ravel)

if(RAVEL_SHARED)
  # Windows names the shared library's import library ravel.lib too, so the
  # static one steps aside when both are built.
  if(WIN32)
    set_target_properties(ravel PROPERTIES OUTPUT_NAME ravel_static)
  endif()

  add_library(ravel_shared SHARED ${RAVEL_SOURCES})
  add_library(ravel::shared ALIAS ravel_shared)
  set_target_properties(ravel_shared PROPERTIES OUTPUT_NAME ravel)
  ravel_configure_library(ravel_shared)
  target_compile_definitions(ravel_shared PRIVATE RAVEL_BUILD_SHARED PUBLIC RAVEL_SHARED)
endif()

if(RAVEL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RAVEL_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(RAVEL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
