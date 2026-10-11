add_library(cutoff_sim STATIC sim_core.cpp)
target_include_directories(cutoff_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_sim PUBLIC cutoff_model)

set(CUTOFF_RTL ${PROJECT_SOURCE_DIR}/rtl)
verilate(cutoff_sim
  SOURCES ${CUTOFF_RTL}/cutoff_core.v ${CUTOFF_RTL}/picorv32/picorv32.v
  TOP_MODULE cutoff_core
  PREFIX Vcutoff_core
  VERILATOR_ARGS --public-flat-rw --no-timing --x-assign 0 --x-initial 0 -O2)

# -Wall -Wextra apply to our own source only; Verilator's generated model is third-party code.
set(CUTOFF_OWN_FLAGS -Wall -Wextra)
if(CUTOFF_SANITIZE)
  list(APPEND CUTOFF_OWN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(cutoff_sim PUBLIC -fsanitize=address,undefined)
endif()
set_source_files_properties(sim_core.cpp PROPERTIES COMPILE_OPTIONS "${CUTOFF_OWN_FLAGS}")

# ---------------------------------------------------------------------------------------------------------------------
# The instrumented ("drill") core: picorv32 with every mutation selectable through mut_sel, generated from the pinned,
# unmodified picorv32.v by cutoff-mutgen. Verilator's CMake helper runs at configure time, so the generator is compiled
# and run at configure time too (it is a single small tool with no dependencies), and CMake reconfigures whenever the
# core, the wrapper, the region table or the generator sources change.
set(DRILL_DIR ${PROJECT_SOURCE_DIR}/drill)
set(DRILL_GEN ${CMAKE_BINARY_DIR}/generated)
set(DRILL_TOOL ${CMAKE_BINARY_DIR}/tools/cutoff-mutgen-bootstrap)
file(MAKE_DIRECTORY ${DRILL_GEN} ${CMAKE_BINARY_DIR}/tools)
set(DRILL_TOOL_SOURCES lexer.cpp expr.cpp walker.cpp regions.cpp mutator.cpp generate.cpp mutgen_main.cpp)
set(DRILL_INPUTS ${CUTOFF_RTL}/picorv32/picorv32.v ${CUTOFF_RTL}/cutoff_core.v ${DRILL_DIR}/picorv32_regions.txt)
foreach(f ${DRILL_TOOL_SOURCES} lexer.hpp expr.hpp walker.hpp regions.hpp mutator.hpp generate.hpp)
  list(APPEND DRILL_TOOL_DEPS ${DRILL_DIR}/${f})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DRILL_INPUTS} ${DRILL_TOOL_DEPS})

set(_rebuild_tool FALSE)
if(NOT EXISTS ${DRILL_TOOL})
  set(_rebuild_tool TRUE)
else()
  foreach(f ${DRILL_TOOL_DEPS})
    if(${f} IS_NEWER_THAN ${DRILL_TOOL})
      set(_rebuild_tool TRUE)
    endif()
  endforeach()
endif()
if(_rebuild_tool)
  message(STATUS "Compiling the mutant generator")
  set(_srcs)
  foreach(f ${DRILL_TOOL_SOURCES})
    list(APPEND _srcs ${DRILL_DIR}/${f})
  endforeach()
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -O1 -I${DRILL_DIR} ${_srcs} -o ${DRILL_TOOL}
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Cannot compile the mutant generator:\n${_err}")
  endif()
endif()
execute_process(COMMAND ${DRILL_TOOL}
    --rtl ${CUTOFF_RTL}/picorv32/picorv32.v --wrapper ${CUTOFF_RTL}/cutoff_core.v --regions ${DRILL_DIR}/picorv32_regions.txt
    --out-rtl ${DRILL_GEN}/picorv32_drill.v --out-manifest ${DRILL_GEN}/picorv32_mutants.tsv --out-wrapper ${DRILL_GEN}/cutoff_core_drill.v
    --out-embed ${DRILL_GEN}/drill_embedded.cpp
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Mutant generation failed:\n${_err}")
endif()
string(REGEX MATCH "^[^\n]*" _first_line "${_out}")
message(STATUS "Instrumented core: ${_first_line}")

add_library(cutoff_sim_drill STATIC sim_drill.cpp ${DRILL_GEN}/drill_embedded.cpp)
target_include_directories(cutoff_sim_drill PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutoff_sim_drill PUBLIC cutoff_model)
verilate(cutoff_sim_drill
  SOURCES ${DRILL_GEN}/cutoff_core_drill.v ${DRILL_GEN}/picorv32_drill.v
  TOP_MODULE cutoff_core_drill
  PREFIX Vcutoff_drill
  VERILATOR_ARGS --public-flat-rw --no-timing --x-assign 0 --x-initial 0 -O2)
if(CUTOFF_SANITIZE)
  target_link_options(cutoff_sim_drill PUBLIC -fsanitize=address,undefined)
endif()
set_source_files_properties(sim_drill.cpp PROPERTIES COMPILE_OPTIONS "${CUTOFF_OWN_FLAGS}")
