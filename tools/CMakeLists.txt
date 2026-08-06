# Local development tools. Not part of the shipped libraries.
add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE match match_warnings)

add_executable(lobster_oracle lobster_oracle.cpp)
target_link_libraries(lobster_oracle PRIVATE adapters match_warnings)

add_executable(null_order_invariance null_order_invariance.cpp)
target_link_libraries(null_order_invariance PRIVATE adapters match_warnings)

# abi_replay drives the adapter through dlopen, so it needs the benchmark's ABI
# header too — same gate as the adapter itself.
if(MATCH_BENCHMARK_DIR AND EXISTS "${MATCH_BENCHMARK_DIR}/api/matching_engine_api.h")
  add_executable(abi_replay abi_replay.cpp)
  target_include_directories(abi_replay PRIVATE "${MATCH_BENCHMARK_DIR}/api")
  target_link_libraries(abi_replay PRIVATE match match_warnings ${CMAKE_DL_LIBS})
endif()
