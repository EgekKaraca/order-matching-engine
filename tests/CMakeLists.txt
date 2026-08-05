include(FetchContent)

# Pinned, not floating: a moving test framework turns an unrelated upstream
# change into a red build on a branch that did not touch anything.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(match_tests
  types_test.cpp
  canonical_test.cpp)

target_link_libraries(match_tests PRIVATE match match_warnings GTest::gtest_main)

gtest_discover_tests(match_tests)
