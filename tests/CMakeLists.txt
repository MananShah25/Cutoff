add_executable(cutoff_tests smoke_test.cpp doctor_test.cpp model_test.cpp model_vs_core_test.cpp)
target_link_libraries(cutoff_tests PRIVATE cutoff_cli cutoff_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cutoff_tests DISCOVERY_MODE PRE_TEST)
