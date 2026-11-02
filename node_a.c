#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#define UART_ID     uart0
#define BAUD_RATE   9600
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define PIN_M0      2
#define PIN_M1      3
#define PIN_AUX     4
#define LED_PIN     PICO_DEFAULT_LED_PIN

// Wait until E32 finishes internal processing or RF transmission
void e32_wait_aux() {
    while (gpio_get(PIN_AUX) == 0) {
        tight_loop_contents();
    }
    sleep_ms(5); // Small guard delay recommended by datasheet
}
/*
ABOUT THE TIGHT_LOOP_CONTENTS() FUNCTION:
is a Pico SDK function, and on the RP2040 it does nothing. It's an empty static inline function in pico/platform.h, so the compiler removes it entirely.

In node_a.c:18-20 it's the body of a busy-wait loop:

while (gpio_get(PIN_AUX) == 0) {
    tight_loop_contents();
}
The loop keeps checking the E32's AUX pin. While AUX is low, the module is busy (sending over RF or processing internally). The loop exits when AUX goes high, meaning the module is ready.

There are two reasons to put it there instead of leaving the loop empty:

Readability: it makes clear the empty loop is intentional, not a mistake.
Future use: the SDK uses it as a hook. Some builds, platforms or debug setups can define it to do something useful in spin loops, like a nop, a watchdog hook, or a power or debugger hint. Your code picks that up without any changes.
*/

void e32_init() {
    // Configure M0 and M1 for Mode 0 (Normal/Transparent: M0=0, M1=0)
    gpio_init(PIN_M0);
    gpio_set_dir(PIN_M0, GPIO_OUT);
    gpio_put(PIN_M0, 0);

    gpio_init(PIN_M1);
    gpio_set_dir(PIN_M1, GPIO_OUT);
    gpio_put(PIN_M1, 0);

    // Configure AUX pin with pull-up
    gpio_init(PIN_AUX);
    gpio_set_dir(PIN_AUX, GPIO_IN);
    gpio_pull_up(PIN_AUX);

    // Initialize UART0 at 9600 8N1 (E32 default)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);

    sleep_ms(100);
    e32_wait_aux();
}

int main() {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Give time to open the USB Serial Monitor in VS Code
    sleep_ms(5000);
    printf("--- Node A (Initiator) Initializing ---\n");

    e32_init();
    printf("E32 Ready. Starting Ping-Pong loop...\n");

    int32_t tx_number = 1;
    int32_t rx_reply  = 0;

    while (true) {
        // Flush any stale bytes in UART RX FIFO before transmitting
        while (uart_is_readable(UART_ID)) {
            uart_getc(UART_ID);
        }

        e32_wait_aux();
        printf("\n[Node A] Sending number: %ld\n", tx_number);

        // Send the 4-byte integer directly over UART
        uart_write_blocking(UART_ID, (const uint8_t *)&tx_number, sizeof(tx_number));
        gpio_put(LED_PIN, 1);

        // Wait up to 1,500,000 microseconds (1.5 seconds) for Node B to reply
        if (uart_is_readable_within_us(UART_ID, 1500000)) {
            uart_read_blocking(UART_ID, (uint8_t *)&rx_reply, sizeof(rx_reply));
            printf("[Node A] Received return message from Node B: %ld\n", rx_reply);
        } else {
            printf("[Node A] Timeout waiting for Node B reply.\n");
        }

        gpio_put(LED_PIN, 0);
        tx_number++;

        sleep_ms(2000);
    }
    return 0;
}