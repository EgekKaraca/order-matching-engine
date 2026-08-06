# Market-data adapters: LOBSTER / ITCH / Databento -> the engine's vocabulary.
add_library(adapters
  src/lobster.cpp)

target_include_directories(adapters PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(adapters PUBLIC match sim)
target_link_libraries(adapters PRIVATE match_warnings)

add_library(match::adapters ALIAS adapters)

# The benchmark harness adapter is separate: it is a dlopen()ed module, not a
# library anything links, and it needs that repo's api/matching_engine_api.h.
# Point at a checkout with -DMATCH_BENCHMARK_DIR=/path/to/matching-engine-benchmark.
if(NOT MATCH_BENCHMARK_DIR)
  return()
endif()

set(_api "${MATCH_BENCHMARK_DIR}/api/matching_engine_api.h")
if(NOT EXISTS "${_api}")
  message(WARNING "MATCH_BENCHMARK_DIR set but ${_api} not found — skipping the harness adapter")
  return()
endif()

add_library(ome_adapter MODULE benchmark_adapter.cpp)
target_include_directories(ome_adapter PRIVATE "${MATCH_BENCHMARK_DIR}/api")
target_link_libraries(ome_adapter PRIVATE match match_warnings)
set_target_properties(ome_adapter PROPERTIES
  PREFIX ""
  POSITION_INDEPENDENT_CODE ON)
