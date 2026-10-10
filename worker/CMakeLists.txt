add_library(cutoff_sim STATIC sim_core.cpp)
target_include_directories(cutoff_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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
