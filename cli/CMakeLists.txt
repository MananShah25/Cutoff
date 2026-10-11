add_library(cutoff_cli STATIC doctor.cpp run_command.cpp)
target_include_directories(cutoff_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_cli PUBLIC cutoff_sim cutoff_engine PRIVATE cutoff_warnings)

add_executable(cutoff main.cpp)
target_link_libraries(cutoff PRIVATE cutoff_cli cutoff_warnings)
