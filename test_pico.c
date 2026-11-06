// test_pico.c - E32 LoRa diagnostic for the Raspberry Pi Pico.
//
// Pico version of Transmitter/test_connection.cpp. Same wiring as node_a.c:
//
//   E32 RXD <- GP0 (UART0 TX)      E32 M0  <- GP2
//   E32 TXD -> GP1 (UART0 RX)      E32 M1  <- GP3
//                                  E32 AUX -> GP4
//
// [1] AUX: checks with a pull-down that the module really drives AUX high.
//     (node_a.c uses a pull-up, which hides a missing AUX wire.)
// [2] Mode 3 (M0=1, M1=1): reads the module settings (C1 C1 C1) and model (C3 C3 C3).
//     Compare them with the Pi's test_connection output.
// [3] Mode 0 (M0=0, M1=0): sends C1 C1 C1 again. Now the module must NOT answer,
//     because in mode 0 it transmits those bytes over the air. If it still answers,
//     M0/M1 aren't reaching it (the E32 pulls them high when they're unconnected).
// [4] Radio test: every 3 s sends a 4-byte number like node_a.c, and prints in hex
//     every byte that arrives. Run node_b_pi3 (or hexdump) on the Pi at the same time.
//
// Output goes to USB serial. Press 'r' in the terminal to run the checks again.

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#define UART_ID     uart0
#define BAUD_RATE   9600
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define PIN_M0      2
#define PIN_M1      3
#define PIN_AUX     8

#define AUX_TIMEOUT_MS      2000
#define REPLY_TIMEOUT_US    1000000  // wait this long for the first byte of a reply
#define GAP_TIMEOUT_US      100000   // a silence this long ends a reply
#define TX_PERIOD_MS        3000
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

// Sets M0/M1 and waits for the module to finish switching mode.
static bool set_mode(int m0, int m1) {
    gpio_put(PIN_M0, m0);
    gpio_put(PIN_M1, m1);
    sleep_ms(50);  // give the module time to see the change and pull AUX low
    bool ok = wait_aux(AUX_TIMEOUT_MS);
    flush_rx();
    return ok;
}

// Sends "cmd cmd cmd" and reads the reply into buf.
static int send_command(uint8_t cmd, uint8_t *buf, int max_len) {
    const uint8_t frame[3] = {cmd, cmd, cmd};
    flush_rx();
    uart_write_blocking(UART_ID, frame, sizeof frame);
    return read_reply(buf, max_len, REPLY_TIMEOUT_US);
}

// Decodes the 6-byte answer to C1 C1 C1. Returns false if something will
// stop node_a.c / node_b_pi3 from working.
static bool print_settings(const uint8_t r[6]) {
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
    printf("    Factory default is C0 00 00 1A 17 44.\n");
    printf("    Address, air rate, channel and FEC must match the Pi's module.\n");

    return uart_ok && !fixed;
}

static void run_checks(void) {
    uint8_t r[16];
    int n;

    printf("\n========== E32 check (Pico) ==========\n");

    // [1] AUX wiring. With a pull-down, AUX only reads high if the module drives it.
    printf("\n[1] AUX on GP4\n");
    gpio_pull_down(PIN_AUX);
    set_mode(0, 0);
    sleep_ms(10);
    const bool aux_ok = wait_aux(AUX_TIMEOUT_MS);
    if (aux_ok) {
        printf("    OK: the module drives AUX high.\n");
    } else {
        printf("    FAIL: AUX stays low. GP4 isn't connected to AUX, the module has no\n"
               "    power, or AUX is set to open-drain. Using a pull-up from here on.\n");
        gpio_pull_up(PIN_AUX);
    }

    // [2] Mode 3: read settings and model.
    printf("\n[2] Mode 3 (M0=1, M1=1): reading the module settings\n");
    if (!set_mode(1, 1)) printf("    Warning: AUX didn't go high after switching mode.\n");

    n = send_command(0xC1, r, 6);
    printf("    C1 C1 C1 ->");
    print_hex(r, n);
    printf("  (%d bytes)\n", n);

    const bool settings_ok = (n == 6) && (r[0] == 0xC0 || r[0] == 0xC2);
    bool config_ok = false;
    if (settings_ok) {
        config_ok = print_settings(r);
    } else if (n == 0) {
        printf("    FAIL: no answer. The Pico isn't talking to its E32. Check:\n"
               "      - GP0 -> E32 RXD and GP1 <- E32 TXD (try swapping them)\n"
               "      - GP2 -> M0 and GP3 -> M1\n"
               "      - E32 VCC, and GND shared with the Pico\n");
    } else {
        printf("    FAIL: unexpected answer. Usually a wiring or baud-rate problem.\n");
    }

    if (settings_ok) {
        wait_aux(AUX_TIMEOUT_MS);
        n = send_command(0xC3, r, sizeof r);
        printf("    C3 C3 C3 ->");
        print_hex(r, n);
        if (n >= 2 && r[0] == 0xC3) {
            printf("  (model byte 0x%02X%s)\n", r[1], r[1] == 0x32 ? " = 433 MHz" : "");
            printf("    Both modules must have the same model byte (same frequency band).\n");
        } else {
            printf("  (no model info; some firmware versions don't answer this)\n");
        }
    }

    // [3] Mode 0: the module must stay quiet. If it answers, M0/M1 didn't reach it.
    printf("\n[3] Mode 0 (M0=0, M1=0): the module must NOT answer now\n");
    if (!set_mode(0, 0)) printf("    Warning: AUX didn't go high after switching mode.\n");

    n = send_command(0xC1, r, 6);
    printf("    C1 C1 C1 ->");
    print_hex(r, n);
    printf("  (%d bytes)\n", n);

    bool mode_ok = false;
    if (!settings_ok) {
        printf("    Skipped: step [2] failed, so this result means nothing.\n");
    } else if (n == 0) {
        mode_ok = true;
        printf("    OK: no answer, so M0/M1 work. (C1 C1 C1 went out over the air.)\n");
    } else if (n == 6 && r[0] == 0xC0) {
        printf("    FAIL: the module is still in mode 3. M0/M1 aren't reaching it;\n"
               "    check the wires on GP2 and GP3.\n");
    } else {
        printf("    Unclear: these bytes may be radio traffic from the other node.\n"
               "    Stop the Pi program and press 'r' to run the checks again.\n");
    }

    printf("\n---------- Summary ----------\n");
    printf("  AUX wire        : %s\n", aux_ok ? "OK" : "FAIL");
    printf("  Pico <-> E32    : %s\n", settings_ok ? "OK" : "FAIL");
    printf("  Settings        : %s\n", !settings_ok ? "unknown" : config_ok ? "OK" : "FAIL");
    printf("  M0/M1 wires     : %s\n", !settings_ok ? "unknown" : mode_ok ? "OK" : "FAIL");
    printf("-----------------------------\n");
}

static void print_radio_banner(void) {
    printf("\n[4] Radio test: sending a 4-byte number every %d s, like node_a.c.\n", TX_PERIOD_MS / 1000);
    printf("    On the Pi run node_b_pi3, or:\n"
           "      stty -F /dev/serial0 9600 raw -echo && hexdump -C /dev/serial0\n"
           "    Press 'r' here to run the checks again.\n\n");
}

static void print_time(void) {
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    printf("[%5lu.%lu s]", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
}

static void hw_init(void) {
    gpio_init(PIN_M0);
    gpio_set_dir(PIN_M0, GPIO_OUT);
    gpio_put(PIN_M0, 0);

    gpio_init(PIN_M1);
    gpio_set_dir(PIN_M1, GPIO_OUT);
    gpio_put(PIN_M1, 0);

    gpio_init(PIN_AUX);
    gpio_set_dir(PIN_AUX, GPIO_IN);
    gpio_pull_down(PIN_AUX);

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(UART_RX_PIN);  // keeps RX idle-high if the E32 TXD wire is loose
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);
}

int main(void) {
    stdio_init_all();
    hw_init();

    // Wait for a terminal so the report isn't lost (start anyway after a while).
    absolute_time_t usb_deadline = make_timeout_time_ms(USB_WAIT_MS);
    while (!stdio_usb_connected() && !time_reached(usb_deadline)) sleep_ms(100);
    sleep_ms(500);

    run_checks();
    print_radio_banner();

    int32_t tx_number = 1;
    int32_t last_sent = 0;
    absolute_time_t next_tx = get_absolute_time();
    uint8_t rx[64];

    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'r' || c == 'R') {
            run_checks();
            print_radio_banner();
            next_tx = get_absolute_time();
        }

        if (time_reached(next_tx)) {
            if (!wait_aux(AUX_TIMEOUT_MS)) {
                print_time();
                printf(" Warning: AUX low for %d ms before sending.\n", AUX_TIMEOUT_MS);
            }
            uint8_t bytes[sizeof tx_number];
            memcpy(bytes, &tx_number, sizeof bytes);
            uart_write_blocking(UART_ID, bytes, sizeof bytes);

            print_time();
            printf(" TX %ld ->", (long)tx_number);
            print_hex(bytes, sizeof bytes);
            printf("\n");

            last_sent = tx_number++;
            next_tx = make_timeout_time_ms(TX_PERIOD_MS);
        }

        if (uart_is_readable(UART_ID)) {
            int n = read_reply(rx, sizeof rx, GAP_TIMEOUT_US);
            print_time();
            printf(" RX (%d bytes):", n);
            print_hex(rx, n);
            if (n == 4) {
                int32_t value;
                memcpy(&value, rx, sizeof value);
                printf("  = %ld", (long)value);
                if (value == last_sent + 1000) printf("  <-- reply from node_b_pi3, link works!");
            }
            printf("\n");
        }
    }
}
