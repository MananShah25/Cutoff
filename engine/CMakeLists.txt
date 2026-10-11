set(CUTOFF_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/cutoff_version.hpp)
add_custom_target(cutoff_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${CUTOFF_VERSION_HEADER} -P ${PROJECT_SOURCE_DIR}/cmake/gen_version.cmake
  BYPRODUCTS ${CUTOFF_VERSION_HEADER}
  COMMENT "Recording git commit")

add_library(cutoff_engine STATIC
  protocol.cpp toml_lite.cpp campaign_config.cpp check.cpp worker.cpp campaign.cpp results.cpp checkpoint.cpp risk_monitor.cpp drill_select.cpp drill_tally.cpp drill_report.cpp system_info.cpp run.cpp)
add_dependencies(cutoff_engine cutoff_version)
target_include_directories(cutoff_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(cutoff_engine PUBLIC cutoff_gen cutoff_model cutoff_risk cutoff_sim cutoff_sim_drill cutoff_sim_infect PRIVATE cutoff_warnings)
