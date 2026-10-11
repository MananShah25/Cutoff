add_library(cutoff_risk STATIC species.cpp estimators.cpp tracker.cpp stop_rule.cpp)
target_include_directories(cutoff_risk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_risk PUBLIC cutoff_model PRIVATE cutoff_warnings)
