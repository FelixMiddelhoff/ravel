# ravel — deterministic simulation testing for C++ distributed systems.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(ravel LANGUAGES CXX VERSION 0.3.0)

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
option(RAVEL_BUILD_FUZZ     "Build the fuzz targets and replay their corpora as tests" ${RAVEL_STANDALONE})
option(RAVEL_FUZZ           "Link the fuzz targets with libFuzzer and sanitizers (Clang)" OFF)
option(RAVEL_WERROR         "Treat compiler warnings as errors (CI)" OFF)
option(RAVEL_COVERAGE       "Instrument for line/branch coverage (GCC or Clang)" OFF)

set(RAVEL_SOURCES
  src/c_api.cpp
  src/determinism.cpp
  src/disk.cpp
  src/network.cpp
  src/rng.cpp
  src/runner.cpp
  src/scheduler.cpp
  src/shrink.cpp
  src/simulation.cpp
  src/sweep.cpp
  src/trace.cpp
  src/version.cpp
)

# Strict warnings for every target we build, not just the library.
function(ravel_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(RAVEL_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
      # Designated initializers deliberately leave defaulted members out.
      -Wno-missing-field-initializers)
    if(RAVEL_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Declared once so the static and shared variants can never drift apart.
function(ravel_configure_library target)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_features(${target} PUBLIC cxx_std_20)
  # GCC 10 only ships <coroutine> behind this flag; from GCC 11 it is on with C++20.
  target_compile_options(${target} PUBLIC
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,11>>:-fcoroutines>)
  ravel_warnings(${target})
  target_link_libraries(${target} PUBLIC Threads::Threads)
  if(RAVEL_FUZZ)
    # Instrument the library so libFuzzer can see which of its branches an
    # input reaches. (The fuzz targets themselves link the fuzzer runtime.)
    target_compile_options(${target} PUBLIC -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endif()
  if(RAVEL_COVERAGE)
    # Public, so the test programs that link the library are instrumented and
    # write their counters at exit.
    target_compile_options(${target} PUBLIC --coverage -fprofile-update=atomic -O0 -g)
    target_link_options(${target} PUBLIC --coverage)
  endif()
endfunction()

find_package(Threads REQUIRED)

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
  set_target_properties(ravel_shared PROPERTIES EXPORT_NAME shared)  # ravel::shared
endif()

# Installation, and a CMake package so consumers can find_package(ravel) and
# link ravel::ravel (or ravel::shared). Skipped when ravel is a subdirectory.
if(RAVEL_STANDALONE)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  set(RAVEL_INSTALL_TARGETS ravel)
  if(RAVEL_SHARED)
    list(APPEND RAVEL_INSTALL_TARGETS ravel_shared)
  endif()

  install(TARGETS ${RAVEL_INSTALL_TARGETS} EXPORT ravelTargets)
  install(DIRECTORY include/ravel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT ravelTargets NAMESPACE ravel:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravel)

  configure_package_config_file(cmake/ravelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ravelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravel)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ravelConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)  # Pre-1.0: minor versions may break.
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ravelConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ravelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravel)
endif()

# API reference: `cmake --build build --target ravel_docs` (needs Doxygen).
find_package(Doxygen QUIET)
if(RAVEL_STANDALONE AND DOXYGEN_FOUND)
  add_custom_target(ravel_docs
    COMMAND ${DOXYGEN_EXECUTABLE} docs/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating the API reference with Doxygen"
    VERBATIM)
endif()

if(RAVEL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RAVEL_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

if(RAVEL_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(RAVEL_BUILD_EXAMPLES)
  add_subdirectory(examples)
  add_subdirectory(docs/snippets)  # The code and output the docs quote.
endif()
