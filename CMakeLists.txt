# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

# Include build functions from Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Set the name of project (as PROJECT_NAME) and c/cpp standards
project(pico_e32_lora C CXX ASM)
SET(CMAKE_C_STANDARD 11)
SET(CMAKE_CXX_STANDARD 17)  

# Creates a pico-sdk subdirectory and initializes it
pico_sdk_init()

# Target 1: Node A (Initiator / Sender) 
add_executable(lora_node_a node_a.c)
target_link_libraries(lora_node_a pico_stdlib hardware_uart)
pico_enable_stdio_usb(lora_node_a 1)
pico_enable_stdio_uart(lora_node_a 0) # Free up UART0 for the E32
pico_add_extra_outputs(lora_node_a)

# Target 2: Node B (Responder / Echo) 
add_executable(lora_node_b node_b.c)
target_link_libraries(lora_node_b pico_stdlib hardware_uart)
pico_enable_stdio_usb(lora_node_b 1)
pico_enable_stdio_uart(lora_node_b 0) # Free up UART0 for the E32
pico_add_extra_outputs(lora_node_b)

