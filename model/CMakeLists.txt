add_library(cutoff_model STATIC rv32i.cpp model.cpp)
target_include_directories(cutoff_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_model PRIVATE cutoff_warnings)
