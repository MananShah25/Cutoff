add_library(cutoff_drill STATIC lexer.cpp expr.cpp walker.cpp regions.cpp mutator.cpp generate.cpp)
target_include_directories(cutoff_drill PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_drill PRIVATE cutoff_warnings)

add_executable(cutoff-mutgen mutgen_main.cpp)
target_link_libraries(cutoff-mutgen PRIVATE cutoff_drill cutoff_warnings)
