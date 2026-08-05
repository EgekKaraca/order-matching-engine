cmake_minimum_required(VERSION 3.20)

project(order_matching_engine
        VERSION 0.1.0
        DESCRIPTION "A benchmark-conformant matching engine and the L3 execution simulator built on it"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clang-tidy and editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MATCH_BUILD_TESTS "Build the test suite" ON)
option(MATCH_WERROR "Treat warnings as errors (on in CI)" OFF)

add_library(match_warnings INTERFACE)
target_compile_options(match_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${MATCH_WERROR}>:-Werror>)

add_subdirectory(libmatch)

if(MATCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
