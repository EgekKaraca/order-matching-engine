# libsim — replay, injection, latency, accounting. Talks to libmatch; libmatch
# knows nothing about any of this.
add_library(sim
  src/simulator.cpp)

target_include_directories(sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(sim PUBLIC match)
target_link_libraries(sim PRIVATE match_warnings)

add_library(match::sim ALIAS sim)
