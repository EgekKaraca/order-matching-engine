# The study: the fill-model comparison that is the project's actual deliverable.
add_executable(fill_model_study fill_model_study.cpp)
target_link_libraries(fill_model_study PRIVATE adapters sim match_warnings)

# The general entry point: any strategy, any fill model, a performance report.
add_executable(backtest_runner backtest_runner.cpp)
target_link_libraries(backtest_runner PRIVATE adapters sim match_warnings)

# How much of the headline rests on the impact-free assumption.
add_executable(impact_sensitivity impact_sensitivity.cpp)
target_link_libraries(impact_sensitivity PRIVATE adapters sim match_warnings)

# Cross-validation against the published live-trading experiment.
add_executable(queue_validation queue_validation.cpp)
target_link_libraries(queue_validation PRIVATE adapters sim match_warnings)
