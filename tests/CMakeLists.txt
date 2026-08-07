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
  map_book_test.cpp
  property_test.cpp
  engine_equivalence_test.cpp
  fill_probability_test.cpp
  canonical_test.cpp
  lobster_test.cpp
  sim_test.cpp)

# adapters links match PUBLIC, so naming both here makes the linker see it twice.
target_link_libraries(match_tests PRIVATE adapters sim match_warnings GTest::gtest_main)

gtest_discover_tests(match_tests)
