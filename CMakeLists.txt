# Set minimum required version of CMake (Pico SDK 2.x needs 3.13)
cmake_minimum_required(VERSION 3.13)

# Use the Pico SDK inside this folder (./pico-sdk), unless a path was given
# with -DPICO_SDK_PATH=... or the PICO_SDK_PATH environment variable.
if (NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
endif ()

# Include build functions from Pico SDK (must come before project())
include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

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

# Target 3: E32 diagnostic for the Pico (wiring, settings, radio link)
add_executable(test_pico test_pico.c)
target_link_libraries(test_pico pico_stdlib hardware_uart)
pico_enable_stdio_usb(test_pico 1)
pico_enable_stdio_uart(test_pico 0) # Free up UART0 for the E32
pico_add_extra_outputs(test_pico)

# Target 4: toggles M0/M1 every 3 s, to check those wires with a multimeter
add_executable(test_pin_in_pico test_pin_in_pico.c)
target_link_libraries(test_pin_in_pico pico_stdlib)
pico_enable_stdio_usb(test_pin_in_pico 1)
pico_enable_stdio_uart(test_pin_in_pico 0) # Free up UART0 for the E32
pico_add_extra_outputs(test_pin_in_pico)

# Target 5: only step [2] of test_pico - holds mode 3, reads settings and pin levels
add_executable(test_2_pico test_2_pico.c)
target_link_libraries(test_2_pico pico_stdlib hardware_uart hardware_adc)
pico_enable_stdio_usb(test_2_pico 1)
pico_enable_stdio_uart(test_2_pico 0) # Free up UART0 for the E32
pico_add_extra_outputs(test_2_pico)
