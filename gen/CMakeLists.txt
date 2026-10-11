add_library(cutoff_gen STATIC generator.cpp)
target_include_directories(cutoff_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_gen PUBLIC cutoff_model PRIVATE cutoff_warnings)
