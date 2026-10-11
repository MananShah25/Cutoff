add_executable(cutoff_tests smoke_test.cpp doctor_test.cpp model_test.cpp model_vs_core_test.cpp gen_test.cpp gen_vs_core_test.cpp engine_test.cpp risk_test.cpp stop_resume_test.cpp drill_test.cpp drill_sim_test.cpp)
target_link_libraries(cutoff_tests PRIVATE cutoff_cli cutoff_drill cutoff_warnings GTest::gtest_main)
target_compile_definitions(cutoff_tests PRIVATE CUTOFF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(cutoff_tests DISCOVERY_MODE PRE_TEST)
