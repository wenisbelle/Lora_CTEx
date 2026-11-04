cmake_minimum_required(VERSION 3.16)
project(pi3_e32_lora CXX)

# Enforce C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release (-O3) if no build type is specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Locate libgpiod via pkg-config (with fallback to find_library)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GPIOD QUIET libgpiod)
endif()

if(NOT GPIOD_FOUND)
    find_library(GPIOD_LIBRARIES NAMES gpiod REQUIRED)
    find_path(GPIOD_INCLUDE_DIRS NAMES gpiod.h REQUIRED)
endif()

# Build the responder executable
add_executable(node_b_pi3 node_b_pi3.cpp)

target_include_directories(node_b_pi3 PRIVATE ${GPIOD_INCLUDE_DIRS})
target_link_libraries(node_b_pi3 PRIVATE ${GPIOD_LIBRARIES})
target_compile_options(node_b_pi3 PRIVATE -Wall -Wextra -O2)