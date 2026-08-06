# The study: the fill-model comparison that is the project's actual deliverable.
add_executable(fill_model_study fill_model_study.cpp)
target_link_libraries(fill_model_study PRIVATE adapters sim match_warnings)
