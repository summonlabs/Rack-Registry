# Rack Registry - independent downstream consumer.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately not part of the Rack Registry build. It exists to
# prove that the installed package is usable by a consumer that has never seen
# this source tree: it is configured against an install prefix, finds the
# package with find_package, builds from sources that live elsewhere and runs.
#
# Configure it with:
#   cmake -S tests/downstream -B <build-dir> -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_PREFIX_PATH=<install-prefix>

cmake_minimum_required(VERSION 3.20)

project(RackRegistryDownstreamConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the installed Rack Registry package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RackRegistry 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE RackRegistry::rack_registry)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME downstream_consumer_runs COMMAND downstream_consumer)
