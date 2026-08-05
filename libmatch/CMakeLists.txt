# libmatch — the matching engine. Knows nothing about backtesting.
add_library(match
  src/canonical.cpp)

target_include_directories(match PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(match PRIVATE match_warnings)

add_library(match::match ALIAS match)
