// test_pin_in_pico.c - toggles the E32 M0/M1 pins so you can check them with a multimeter.
//
// Wiring (same as node_a.c, except AUX is on GP10 here):
//
//   E32 RXD <- GP0  (physical pin 1),  held high: the UART idle level
//   E32 TXD -> GP1  (physical pin 2),  not used
//   E32 M0  <- GP2  (physical pin 4)
//   E32 M1  <- GP3  (physical pin 5)
//   E32 AUX -> GP10 (physical pin 14), only read and printed
//
// Every 3 s, M0 and M1 switch together between high (3.3 V) and low (0 V).
// Measure from the M0 (or M1) pin ON THE E32 MODULE to GND:
//   - it follows the Pico (3.3 V / 0 V every 3 s)  -> the wire works
//   - it stays high all the time                    -> the wire is broken or on the wrong pin
//     (the E32 has internal pull-ups on M0/M1, so an unconnected pin reads high)
//
// GP0 has to be driven high. This program doesn't use the UART, and an unconfigured RP2040
// pin has a pull-down, which holds the E32's RXD low. The module reads that as data arriving,
// stays busy, and keeps AUX low.
//
// The on-board LED mirrors M0/M1, so you can see the program running without a terminal.
// USB serial prints the current state and the AUX reading.

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"

#define PIN_E32_RXD         0   // Pico TX -> E32 RXD
#define PIN_M0              2
#define PIN_M1              3
#define PIN_AUX             8
#define TOGGLE_PERIOD_MS    3000

static void init_output(uint pin, bool value) {
    gpio_init(pin);
    gpio_put(pin, value);  // set the level before enabling the output, so there's no glitch
    gpio_set_dir(pin, GPIO_OUT);
}

int main(void) {
    stdio_init_all();

    init_output(PIN_E32_RXD, 1);  // UART idle level, so the E32 doesn't see a never-ending start bit
    init_output(PIN_M0, 0);
    init_output(PIN_M1, 0);
#ifdef PICO_DEFAULT_LED_PIN
    init_output(PICO_DEFAULT_LED_PIN, 0);
#endif

    gpio_init(PIN_AUX);
    gpio_set_dir(PIN_AUX, GPIO_IN);
    gpio_pull_down(PIN_AUX);  // AUX reads 1 only if the module really drives it high

    bool level = true;
    while (true) {
        gpio_put(PIN_M0, level);
        gpio_put(PIN_M1, level);
#ifdef PICO_DEFAULT_LED_PIN
        gpio_put(PICO_DEFAULT_LED_PIN, level);
#endif

        sleep_ms(100);  // let the module react to the mode change before reading AUX
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        printf("[%5lu.%lu s] M0 (GP2) = M1 (GP3) = %s   AUX (GP10) = %d\n",
               (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100),
               level ? "HIGH (3.3 V) -> mode 3, sleep " : "LOW  (0 V)   -> mode 0, normal",
               gpio_get(PIN_AUX));

        level = !level;
        sleep_ms(TOGGLE_PERIOD_MS - 100);
    }
}
