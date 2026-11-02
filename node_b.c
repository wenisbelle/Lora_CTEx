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

void e32_wait_aux() {
    while (gpio_get(PIN_AUX) == 0) {
        tight_loop_contents();
    }
    sleep_ms(5);
}

void e32_init() {
    gpio_init(PIN_M0);
    gpio_set_dir(PIN_M0, GPIO_OUT);
    gpio_put(PIN_M0, 0);

    gpio_init(PIN_M1);
    gpio_set_dir(PIN_M1, GPIO_OUT);
    gpio_put(PIN_M1, 0);

    gpio_init(PIN_AUX);
    gpio_set_dir(PIN_AUX, GPIO_IN);
    gpio_pull_up(PIN_AUX);

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

    sleep_ms(5000);
    printf("--- Node B (Responder) Initializing ---\n");

    e32_init();
    printf("E32 Ready. Listening for incoming packets...\n");

    int32_t received_number = 0;
    int32_t return_number   = 0;

    while (true) {
        // Check if data has arrived in the UART FIFO
        if (uart_is_readable(UART_ID)) {
            gpio_put(LED_PIN, 1);

            // Read the 4-byte integer sent by Node A
            uart_read_blocking(UART_ID, (uint8_t *)&received_number, sizeof(received_number));
            printf("\n[Node B] Received: %ld\n", received_number);

            // Wait for E32 to finish outputting UART data and return to idle state
            e32_wait_aux();
            sleep_ms(20); // Brief turnaround delay before transmitting

            // Compute return message (e.g., add 1000 to verify modification)
            return_number = received_number + 1000;

            printf("[Node B] Sending return message: %ld\n", return_number);
            uart_write_blocking(UART_ID, (const uint8_t *)&return_number, sizeof(return_number));

            e32_wait_aux();
            gpio_put(LED_PIN, 0);
        }
        tight_loop_contents();
    }
    return 0;
}