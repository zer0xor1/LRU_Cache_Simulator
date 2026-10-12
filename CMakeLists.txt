cmake_minimum_required(VERSION 3.16)
project(cachesim VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CACHESIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CACHESIM_WERROR "Treat compiler warnings as errors" OFF)
option(CACHESIM_BUILD_TESTS "Build the test suite" ON)

find_package(Threads REQUIRED)

# ---- warnings and sanitizers, applied to every target we own ----------------
add_library(cachesim_options INTERFACE)
if(MSVC)
  target_compile_options(cachesim_options INTERFACE /W4 /permissive-)
  if(CACHESIM_WERROR)
    target_compile_options(cachesim_options INTERFACE /WX)
  endif()
else()
  target_compile_options(cachesim_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                                    -Wno-sign-conversion)
  if(CACHESIM_WERROR)
    target_compile_options(cachesim_options INTERFACE -Werror)
  endif()
  if(CACHESIM_SANITIZE)
    target_compile_options(cachesim_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                      -fno-sanitize-recover=all)
    target_link_options(cachesim_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# ---- core library -------------------------------------------------------------
add_library(cachesim_core STATIC
  src/types.cpp
  src/policy.cpp
  src/cache.cpp
  src/config.cpp
  src/trace.cpp
  src/simulator.cpp
  src/report.cpp
)
target_include_directories(cachesim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cachesim_core PUBLIC cachesim_options)

# ---- command-line tools -----------------------------------------------------
add_executable(cachesim apps/cachesim.cpp)
target_link_libraries(cachesim PRIVATE cachesim_core Threads::Threads)

add_executable(tracegen tools/tracegen.cpp)
target_link_libraries(tracegen PRIVATE cachesim_core)

# ---- tests ------------------------------------------------------------------
if(CACHESIM_BUILD_TESTS)
  enable_testing()
  add_executable(cachesim_tests
    tests/test_main.cpp
    tests/test_flat_hash_map.cpp
    tests/test_config.cpp
    tests/test_trace.cpp
    tests/test_policies.cpp
    tests/test_cache.cpp
    tests/test_simulator.cpp
    tests/test_differential.cpp
    tests/test_regression.cpp
  )
  target_link_libraries(cachesim_tests PRIVATE cachesim_core)
  target_compile_definitions(cachesim_tests PRIVATE CACHESIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  add_test(NAME unit_and_property_tests COMMAND cachesim_tests)

  # End-to-end checks of the CLI against known numbers.
  add_test(NAME cli_original_example
           COMMAND cachesim -q -s 32768 -b 64 -a 4 ${CMAKE_CURRENT_SOURCE_DIR}/traces/example.trc)
  set_tests_properties(cli_original_example PROPERTIES
           PASS_REGULAR_EXPRESSION "L1 miss rate 0\\.145760 \\(101379/695521\\)")
  add_test(NAME cli_legacy_config
           COMMAND cachesim -q ${CMAKE_CURRENT_SOURCE_DIR}/configs/legacy_example.cfg
                   ${CMAKE_CURRENT_SOURCE_DIR}/traces/example.trc)
  set_tests_properties(cli_legacy_config PROPERTIES PASS_REGULAR_EXPRESSION "L1 miss rate")
  add_test(NAME cli_hierarchy_json
           COMMAND cachesim --json -c ${CMAKE_CURRENT_SOURCE_DIR}/configs/intel_like.cfg
                   ${CMAKE_CURRENT_SOURCE_DIR}/traces/samples/matmul_naive_n32.trc)
  set_tests_properties(cli_hierarchy_json PROPERTIES PASS_REGULAR_EXPRESSION "\"amat_cycles\"")
  add_test(NAME cli_rejects_bad_geometry
           COMMAND cachesim -s 128 -b 64 -a 4 ${CMAKE_CURRENT_SOURCE_DIR}/traces/example.trc)
  set_tests_properties(cli_rejects_bad_geometry PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli_sweep
           COMMAND cachesim --sweep --sizes 1K-8K --assocs 1,2,full --policies lru,opt
                   ${CMAKE_CURRENT_SOURCE_DIR}/traces/samples/loop_working_set.trc)
  set_tests_properties(cli_sweep PROPERTIES PASS_REGULAR_EXPRESSION "trace,size_bytes")
endif()
