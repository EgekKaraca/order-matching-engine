# Local development tools. Not part of the shipped libraries.
add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE match match_warnings)
