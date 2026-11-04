cmake_minimum_required(VERSION 3.16)
project(pi3_e32_lora CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(node_b_pi3 node_b_pi3.cpp)
target_compile_options(node_b_pi3 PRIVATE -Wall -Wextra -O2)