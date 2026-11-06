// test_2_pico.c - only step [2] of test_pico.c: keeps the E32 in mode 3 and reads its settings.
//
// Same wiring as test_pico.c:
//
//   E32 RXD <- GP0 (physical pin 1, UART0 TX)    E32 M0  <- GP2 (physical pin 4)
//   E32 TXD -> GP1 (physical pin 2, UART0 RX)    E32 M1  <- GP3 (physical pin 5)
//                                                E32 AUX -> GP8 (physical pin 11)
//
// M0 and M1 stay high (mode 3) for the whole run, so every voltage is steady and
// you can measure it with a multimeter. In mode 3 the radio is off, so other LoRa
// modules transmitting nearby can't change the result.
//
// Every 2 s it sends C1 C1 C1 and prints one line with:
//   - the reply (6 bytes starting with C0 = OK)
//   - what the Pico reads on M0, M1, AUX and GP1 (E32 TXD)
//   - the Pico's VSYS voltage (ADC on GP29)
// The decoded settings are printed again whenever the reply changes, so you can
// wiggle wires and watch.
//
// Output goes to USB serial.

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

#define UART_ID     uart0
#define BAUD_RATE   9600
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define PIN_M0      2
#define PIN_M1      3
#define PIN_AUX     8

#define VSYS_ADC_INPUT  3        // GP29 = ADC3 = VSYS / 3 on the Pico board

#define AUX_TIMEOUT_MS      2000
#define REPLY_TIMEOUT_US    1000000  // wait this long for the first byte of a reply
#define GAP_TIMEOUT_US      100000   // a silence this long ends a reply
#define CHECK_PERIOD_MS     2000
#define USB_WAIT_MS         15000    // max wait for a terminal before starting anyway

// Waits until AUX is high (module idle). Returns false on timeout.
static bool wait_aux(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!gpio_get(PIN_AUX)) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    sleep_ms(5);
    return true;
}

static void flush_rx(void) {
    while (uart_is_readable(UART_ID)) uart_getc(UART_ID);
}

// Reads bytes until max_len, or until the line goes quiet. Returns the count.
static int read_reply(uint8_t *buf, int max_len, uint32_t first_byte_timeout_us) {
    int n = 0;
    uint32_t timeout = first_byte_timeout_us;
    while (n < max_len && uart_is_readable_within_us(UART_ID, timeout)) {
        buf[n++] = (uint8_t)uart_getc(UART_ID);
        timeout = GAP_TIMEOUT_US;
    }
    return n;
}

static void print_hex(const uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) printf(" %02X", buf[i]);
}

static void print_time(void) {
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    printf("[%5lu.%lu s]", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
}

// Sends "cmd cmd cmd" and reads the reply into buf.
static int send_command(uint8_t cmd, uint8_t *buf, int max_len) {
    const uint8_t frame[3] = {cmd, cmd, cmd};
    flush_rx();
    uart_write_blocking(UART_ID, frame, sizeof frame);
    return read_reply(buf, max_len, REPLY_TIMEOUT_US);
}

// Reads GP1 with a pull-down for a moment. It reads 1 only if the E32 really drives
// its TXD high; with the normal pull-up a loose wire would also read 1.
static bool e32_txd_driven_high(void) {
    gpio_pull_down(UART_RX_PIN);
    sleep_us(50);
    const bool level = gpio_get(UART_RX_PIN);
    gpio_pull_up(UART_RX_PIN);
    sleep_ms(2);   // if the line dropped, the UART saw a junk byte: let it finish, then drop it
    flush_rx();
    return level;
}

static float read_vsys(void) {
    adc_select_input(VSYS_ADC_INPUT);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    return (sum / 16.0f) * 3.3f / 4096.0f * 3.0f;
}

// Decodes the 6-byte answer to C1 C1 C1.
static void print_settings(const uint8_t r[6]) {
    static const char *const parity_name[] = {"8N1", "8O1", "8E1", "8N1"};
    static const unsigned baud_value[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    static const char *const air_name[] = {"0.3k", "1.2k", "2.4k", "4.8k", "9.6k", "19.2k", "19.2k", "19.2k"};

    const uint8_t sped = r[3], chan = r[4], opt = r[5];
    const unsigned baud = baud_value[(sped >> 3) & 0x07];
    const int parity = sped >> 6;
    const bool uart_ok = (baud == 9600) && (parity == 0 || parity == 3);
    const bool fixed = opt & 0x80;

    printf("    Address   : 0x%02X%02X\n", r[1], r[2]);
    printf("    UART      : %u %s (used in mode 0)%s\n", baud, parity_name[parity],
           uart_ok ? "" : "   <-- PROBLEM: the code uses 9600 8N1");
    printf("    Air rate  : %s bps\n", air_name[sped & 0x07]);
    printf("    Channel   : 0x%02X (%d MHz if this is a 433 MHz module)\n", chan, 410 + (chan & 0x1F));
    printf("    Mode      : %s\n", fixed
           ? "FIXED transmission   <-- PROBLEM: eats the first 3 bytes of every message"
           : "transparent");
    printf("    IO drive  : %s\n", (opt & 0x40) ? "push-pull" : "open-drain");
    printf("    FEC       : %s\n", (opt & 0x04) ? "on" : "off");
    printf("    TX power  : level %d (0 = max)\n", opt & 0x03);
    printf("    Factory default is C0 00 00 1A 17 44.\n\n");
}

static void print_banner(void) {
    printf("\n========== E32 mode 3 check (Pico) ==========\n");
    printf("M0 (GP%d) and M1 (GP%d) are held HIGH: the module stays in mode 3.\n\n", PIN_M0, PIN_M1);
    printf("Multimeter, black probe on GND, red probe on the pin ON THE E32 MODULE:\n");
    printf("  VCC : the supply you wired (E32 range is about 2.3-5.2 V; 5 V is typical)\n");
    printf("  GND : 0 V against Pico GND (pin 3 or 38); anything else = no shared ground\n");
    printf("  M0  : ~3.3 V\n");
    printf("  M1  : ~3.3 V\n");
    printf("  AUX : ~3.3 V (module idle)\n");
    printf("  RXD : ~3.3 V (Pico TX idle level)\n");
    printf("  TXD : ~3.3 V (module TX idle level)\n");
    printf("Note: the E32 pulls M0/M1 high by itself, so in mode 3 a broken M0/M1 wire\n");
    printf("also reads 3.3 V. Use test_pin_in_pico to check those two wires.\n\n");
    printf("Each line: reply | what the Pico reads (1 = high) | Pico VSYS\n\n");
}

static void hw_init(void) {
    gpio_init(PIN_M0);
    gpio_put(PIN_M0, 1);           // set the level before enabling the output, so there's no glitch
    gpio_set_dir(PIN_M0, GPIO_OUT);

    gpio_init(PIN_M1);
    gpio_put(PIN_M1, 1);
    gpio_set_dir(PIN_M1, GPIO_OUT);

    gpio_init(PIN_AUX);
    gpio_set_dir(PIN_AUX, GPIO_IN);
    gpio_pull_down(PIN_AUX);       // AUX reads 1 only if the module really drives it high

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(UART_RX_PIN);     // keeps RX idle-high if the E32 TXD wire is loose
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);

    adc_init();
    adc_gpio_init(PICO_VSYS_PIN);
}

int main(void) {
    stdio_init_all();
    hw_init();

    // Wait for a terminal so the report isn't lost (start anyway after a while).
    absolute_time_t usb_deadline = make_timeout_time_ms(USB_WAIT_MS);
    while (!stdio_usb_connected() && !time_reached(usb_deadline)) sleep_ms(100);
    sleep_ms(500);

    print_banner();

    uint8_t last[6] = {0};
    bool have_last = false;

    while (true) {
        const bool aux_idle = wait_aux(AUX_TIMEOUT_MS);

        uint8_t r[16];
        const int n = send_command(0xC1, r, 6);
        const bool ok = (n == 6) && (r[0] == 0xC0 || r[0] == 0xC2);

        print_time();
        printf(" C1 C1 C1 ->");
        print_hex(r, n);
        printf(" (%d bytes) %s", n, ok ? "OK" : n == 0 ? "NO ANSWER" : "BAD");
        printf(" | M0=%d M1=%d AUX=%d TXD=%d | VSYS=%.2f V\n",
               gpio_get(PIN_M0), gpio_get(PIN_M1), gpio_get(PIN_AUX),
               e32_txd_driven_high(), read_vsys());

        if (!aux_idle) {
            printf("    AUX stayed low for %d ms: GP%d not wired to AUX, module unpowered,\n"
                   "    or AUX set to open-drain.\n", AUX_TIMEOUT_MS, PIN_AUX);
        }
        if (ok && (!have_last || memcmp(r, last, 6) != 0)) {
            print_settings(r);
            memcpy(last, r, 6);
            have_last = true;
        } else if (n == 0) {
            printf("    No answer. Check GP0 -> E32 RXD and GP1 <- E32 TXD (try swapping),\n"
                   "    E32 VCC, and that GND is shared with the Pico.\n");
        } else if (!ok) {
            printf("    Unexpected answer: usually a wiring or baud-rate problem, or M0/M1\n"
                   "    not reaching the module (then it's in mode 0 and may be radio traffic).\n");
        }

        sleep_ms(CHECK_PERIOD_MS);
    }
}
