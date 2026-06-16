cmake_minimum_required(VERSION 3.16)
project(hymeta_evolve CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3 -march=native)

# Fixed infrastructure (harness) compiled once as a static lib.
add_library(hymeta_harness STATIC
  harness/sim_engine.cpp
  harness/workload.cpp
  harness/layout_loader.cpp
)
target_include_directories(hymeta_harness PUBLIC harness)

# The simulator binary links the evolved policy (initial_program.cpp) +
# harness + sim_main entry point. OpenEvolve rebuilds this per candidate by
# swapping initial_program.cpp.
add_executable(sim_main
  initial_program.cpp
  harness/sim_main.cpp
)
target_link_libraries(sim_main PRIVATE hymeta_harness)
