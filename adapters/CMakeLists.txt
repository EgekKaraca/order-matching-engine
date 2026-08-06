# The benchmark harness adapter. Built only when the benchmark repo is
# available, since it needs that repo's api/matching_engine_api.h. Point at it
# with -DMATCH_BENCHMARK_DIR=/path/to/matching-engine-benchmark.
if(NOT MATCH_BENCHMARK_DIR)
  return()
endif()

set(_api "${MATCH_BENCHMARK_DIR}/api/matching_engine_api.h")
if(NOT EXISTS "${_api}")
  message(WARNING "MATCH_BENCHMARK_DIR set but ${_api} not found — skipping the adapter")
  return()
endif()

# MODULE, not SHARED: the harness dlopen()s it, nothing links against it.
add_library(ome_adapter MODULE benchmark_adapter.cpp)
target_include_directories(ome_adapter PRIVATE "${MATCH_BENCHMARK_DIR}/api")
target_link_libraries(ome_adapter PRIVATE match match_warnings)
set_target_properties(ome_adapter PROPERTIES
  PREFIX ""
  POSITION_INDEPENDENT_CODE ON)
