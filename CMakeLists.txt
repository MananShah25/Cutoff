cmake_minimum_required(VERSION 3.24)
project(Cutoff VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CUTOFF_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

add_library(cutoff_warnings INTERFACE)
target_compile_options(cutoff_warnings INTERFACE -Wall -Wextra)
if(CUTOFF_SANITIZE)
  target_compile_options(cutoff_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(cutoff_warnings INTERFACE -fsanitize=address,undefined)
endif()

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()
find_package(verilator 5.0 REQUIRED HINTS $ENV{VERILATOR_ROOT})

include(FetchContent)
FetchContent_Declare(googletest
  SYSTEM
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
  URL_HASH SHA256=40d4ec942217dcc84a9ebe2a68584ada7d4a33a8ee958755763278ea1c5e18ff)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(worker)
add_subdirectory(cli)

enable_testing()
add_subdirectory(tests)
