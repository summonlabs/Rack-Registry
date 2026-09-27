# Rack Registry - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the benchmarks and the tests each carry exactly the
# options they need and nothing this project uses for itself can leak into the
# installed package.

cmake_minimum_required(VERSION 3.20)

project(RackRegistry
        VERSION 1.0.0
        DESCRIPTION "Canonical rack composition runtime for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build rack_registry as a shared library instead of a static one" OFF)

option(RR_BUILD_TESTS "Build the Rack Registry test suite" ON)
option(RR_BUILD_TOOLS "Build the Rack Registry command line tools" ON)
option(RR_BUILD_EXAMPLES "Build the Rack Registry example programs" ON)
option(RR_BUILD_BENCHMARKS "Build the Rack Registry benchmarks" ON)
option(RR_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(RR_SANITIZERS "Build with AddressSanitizer (and UndefinedBehaviorSanitizer on GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(RR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/RackRegistry"
    CACHE STRING "Install destination of the Rack Registry CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it becomes part of the installed package.
function(rr_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(RR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(RR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(RR_SANITIZERS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# Adds one executable and applies the project build policy to it.
function(rr_add_executable target)
  add_executable(${target} ${ARGN})
  rr_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(RR_PUBLIC_HEADERS
  include/rack_registry/compatibility.hpp
  include/rack_registry/diff.hpp
  include/rack_registry/digest.hpp
  include/rack_registry/errors.hpp
  include/rack_registry/export.hpp
  include/rack_registry/ids.hpp
  include/rack_registry/lifecycle.hpp
  include/rack_registry/limits.hpp
  include/rack_registry/member.hpp
  include/rack_registry/mount.hpp
  include/rack_registry/persistence.hpp
  include/rack_registry/provenance.hpp
  include/rack_registry/rack.hpp
  include/rack_registry/rack_registry.hpp
  include/rack_registry/registry.hpp
  include/rack_registry/requests.hpp
  include/rack_registry/result.hpp
  include/rack_registry/text.hpp
  include/rack_registry/version.hpp)

set(RR_SOURCES
  src/compatibility.cpp
  src/diff.cpp
  src/digest.cpp
  src/errors.cpp
  src/lifecycle.cpp
  src/member.cpp
  src/mount.cpp
  src/occupancy.cpp
  src/persistence.cpp
  src/provenance.cpp
  src/rack.cpp
  src/registry.cpp
  src/requests.cpp
  src/serialization.cpp
  src/store_files.cpp
  src/text.cpp
  src/version.cpp)

add_library(rack_registry ${RR_SOURCES} ${RR_PUBLIC_HEADERS})
add_library(RackRegistry::rack_registry ALIAS rack_registry)

target_include_directories(rack_registry
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set_target_properties(rack_registry PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

rr_configure_target(rack_registry)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(rack_registry
    PUBLIC RACK_REGISTRY_SHARED
    PRIVATE RACK_REGISTRY_BUILDING)
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests and benchmarks
# ---------------------------------------------------------------------------

if(RR_BUILD_TOOLS)
  rr_add_executable(rackctl tools/rackctl.cpp)
  target_link_libraries(rackctl PRIVATE RackRegistry::rack_registry)
  set_target_properties(rackctl PROPERTIES OUTPUT_NAME rackctl)

  if(RR_BUILD_TESTS)
    # Process-level proof harnesses. They are real programs that a test starts
    # as separate operating-system processes to inject crashes at durable step
    # boundaries and to compete for writer authority. They are not installed.
    rr_add_executable(rr_crash_child tools/crash_child.cpp)
    target_link_libraries(rr_crash_child PRIVATE RackRegistry::rack_registry)
    rr_add_executable(rr_fence_child tools/fence_child.cpp)
    target_link_libraries(rr_fence_child PRIVATE RackRegistry::rack_registry)
  endif()
endif()

if(RR_BUILD_EXAMPLES)
  foreach(example IN ITEMS
      example_rack_lifecycle
      example_durable_store
      example_occupancy_query)
    rr_add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE RackRegistry::rack_registry)
  endforeach()
endif()

if(RR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(RR_BUILD_BENCHMARKS)
  rr_add_executable(bench_rack_registry benchmarks/bench_rack_registry.cpp)
  target_link_libraries(bench_rack_registry PRIVATE RackRegistry::rack_registry)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS rack_registry
        EXPORT RackRegistryTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/rack_registry
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

if(RR_BUILD_TOOLS)
  install(TARGETS rackctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT RackRegistryTargets
        FILE RackRegistryTargets.cmake
        NAMESPACE RackRegistry::
        DESTINATION ${RR_INSTALL_CMAKEDIR})

configure_package_config_file(
  cmake/RackRegistryConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/RackRegistryConfig.cmake"
  INSTALL_DESTINATION ${RR_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RackRegistryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RackRegistryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RackRegistryConfigVersion.cmake"
        DESTINATION ${RR_INSTALL_CMAKEDIR})

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "Rack Registry ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  shared library      : ${BUILD_SHARED_LIBS}")
message(STATUS "  tests               : ${RR_BUILD_TESTS}")
message(STATUS "  tools               : ${RR_BUILD_TOOLS}")
message(STATUS "  examples            : ${RR_BUILD_EXAMPLES}")
message(STATUS "  benchmarks          : ${RR_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors  : ${RR_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${RR_SANITIZERS}")
