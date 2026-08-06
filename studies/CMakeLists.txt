# The study: the fill-model comparison that is the project's actual deliverable.
add_executable(backtest backtest.cpp)
target_link_libraries(backtest PRIVATE adapters sim match_warnings)
