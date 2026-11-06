cmake_minimum_required(VERSION 3.16)
project(pi3_e32_lora CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Node B on the Pi: answers every number from the Pico with number + 1000
add_executable(node_b_pi3 node_b_pi3.cpp)
target_compile_options(node_b_pi3 PRIVATE -Wall -Wextra -O2)

# Quick check of the Pi <-> E32 wiring: reads the module settings once (mode 3)
add_executable(test_connection test_connection.cpp)
target_compile_options(test_connection PRIVATE -Wall -Wextra -O2)

# Full E32 diagnostic for the Pi (serial setup, wiring, settings, radio link).
# Pi version of ../test_pico.c; run both at the same time for the radio test.
add_executable(test_pi3 test_pi3.cpp)
target_compile_options(test_pi3 PRIVATE -Wall -Wextra -O2)
