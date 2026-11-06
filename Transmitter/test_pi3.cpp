// test_pi3.cpp - E32 LoRa diagnostic for the Raspberry Pi 3.
//
// Pi version of ../test_pico.c, with the same step numbers so the two outputs
// can be compared side by side. Same wiring as node_b_pi3.cpp:
//
//   E32 RXD <- GPIO14 / TXD (pin 8)      E32 M0  <- GPIO23 (pin 16)
//   E32 TXD -> GPIO15 / RXD (pin 10)     E32 M1  <- GPIO24 (pin 18)
//                                        E32 AUX -> GPIO25 (pin 22)
//
// [0] Serial port (Pi only): checks the Linux side of /dev/serial0, the top
//     suspect in notes.md: which UART it points to, whether a login console
//     uses it, and whether another program has it open.
// [1] AUX: checks with a pull-down that the module really drives AUX high.
//     (node_b_pi3 uses a pull-up, which hides a missing AUX wire.)
// [2] Mode 3 (M0=1, M1=1): reads the module settings (C1 C1 C1) and model (C3 C3 C3).
//     Compare them with the Pico's test_pico output.
// [3] Mode 0 (M0=0, M1=0): sends C1 C1 C1 again. Now the module must NOT answer,
//     because in mode 0 it transmits those bytes over the air. If it still answers,
//     M0/M1 aren't reaching it (the E32 pulls them high when they're unconnected).
// [4] Radio test: prints in hex every byte that arrives, and answers every 4-byte
//     number with number + 1000, like node_b_pi3. Run test_pico (or node_a) on the
//     Pico at the same time.
//
// Usage: test_pi3 [--checks-only] [--port DEVICE]
// Type r + Enter to run the checks again, Ctrl+C to quit.

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

constexpr const char* GPIO_CHIP_PATH = "/dev/gpiochip0";
constexpr const char* DEFAULT_PORT   = "/dev/serial0";

// BCM GPIO numbers (physical pins 16, 18, 22)
constexpr uint32_t PIN_M0  = 23;
constexpr uint32_t PIN_M1  = 24;
constexpr uint32_t PIN_AUX = 25;

// Position of each pin inside our GPIO line request
constexpr int IDX_M0  = 0;
constexpr int IDX_M1  = 1;
constexpr int IDX_AUX = 2;

constexpr int AUX_TIMEOUT_MS   = 2000;
constexpr int REPLY_TIMEOUT_MS = 1000;  // wait this long for the first byte of a reply
constexpr int GAP_TIMEOUT_MS   = 100;   // a silence this long ends a reply

static int  g_line_fd = -1;              // GPIO line request holding M0, M1 and AUX
static int  g_uart_fd = -1;
static bool g_m0 = false, g_m1 = false;  // current M0/M1 output levels
static const Clock::time_point g_start = Clock::now();

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ------------------------------------------------------------------ GPIO

// Full line config: M0/M1 as outputs at their current levels, AUX as an input
// with the given bias. The kernel wants the whole config on every change.
static gpio_v2_line_config make_gpio_config(bool aux_pull_up) {
    gpio_v2_line_config cfg{};
    cfg.flags     = GPIO_V2_LINE_FLAG_OUTPUT;
    cfg.num_attrs = 2;

    cfg.attrs[0].mask        = (1ULL << IDX_M0) | (1ULL << IDX_M1);
    cfg.attrs[0].attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    cfg.attrs[0].attr.values = (uint64_t(g_m0) << IDX_M0) | (uint64_t(g_m1) << IDX_M1);

    cfg.attrs[1].mask       = 1ULL << IDX_AUX;
    cfg.attrs[1].attr.id    = GPIO_V2_LINE_ATTR_ID_FLAGS;
    cfg.attrs[1].attr.flags = GPIO_V2_LINE_FLAG_INPUT |
        (aux_pull_up ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN);
    return cfg;
}

static bool gpio_open() {
    const int chip = open(GPIO_CHIP_PATH, O_RDONLY | O_CLOEXEC);
    if (chip < 0) {
        printf("Can't open %s: %s\n", GPIO_CHIP_PATH, strerror(errno));
        if (errno == EACCES)
            printf("  Add yourself to the gpio group (sudo usermod -aG gpio $USER) and log in again.\n");
        return false;
    }

    gpio_v2_line_request req{};
    std::strncpy(req.consumer, "e32_test_pi3", sizeof req.consumer - 1);
    req.offsets[IDX_M0]  = PIN_M0;
    req.offsets[IDX_M1]  = PIN_M1;
    req.offsets[IDX_AUX] = PIN_AUX;
    req.num_lines = 3;
    req.config    = make_gpio_config(false);

    const int rc  = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
    const int err = errno;
    close(chip);  // the line request fd stays valid
    if (rc < 0) {
        printf("Can't get GPIO%u/%u/%u: %s\n", PIN_M0, PIN_M1, PIN_AUX, strerror(err));
        if (err == EBUSY)
            printf("  Another program has these pins. Stop node_b_pi3 / test_connection first.\n");
        return false;
    }
    g_line_fd = req.fd;
    return true;
}

static void set_aux_pull(bool pull_up) {
    gpio_v2_line_config cfg = make_gpio_config(pull_up);
    if (ioctl(g_line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) < 0)
        printf("    Warning: can't set the pull-%s on AUX: %s\n", pull_up ? "up" : "down", strerror(errno));
}

static bool read_aux() {
    gpio_v2_line_values v{};
    v.mask = 1ULL << IDX_AUX;
    if (ioctl(g_line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return false;
    return v.bits & (1ULL << IDX_AUX);
}

// Waits until AUX is high (module idle). Returns false on timeout.
static bool wait_aux(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!read_aux()) {
        if (Clock::now() >= deadline) return false;
        sleep_ms(1);
    }
    sleep_ms(5);
    return true;
}

// ------------------------------------------------------------------ UART

static bool uart_open(const char* port) {
    const int fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        printf("Can't open %s: %s\n", port, strerror(errno));
        if (errno == EACCES)
            printf("  Add yourself to the dialout group (sudo usermod -aG dialout $USER) and log in again.\n");
        return false;
    }

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        printf("%s isn't a serial port: %s\n", port, strerror(errno));
        close(fd);
        return false;
    }

    // Raw 9600 8N1, no flow control. Reads return at once; poll() does the waiting.
    cfmakeraw(&tty);
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("Can't configure %s: %s\n", port, strerror(errno));
        close(fd);
        return false;
    }

    // tcsetattr() succeeds if any one change worked, so read the settings back.
    termios now{};
    if (tcgetattr(fd, &now) != 0 || cfgetispeed(&now) != B9600 || cfgetospeed(&now) != B9600 ||
        (now.c_cflag & (CSIZE | PARENB | CSTOPB)) != CS8) {
        printf("Warning: %s didn't accept 9600 8N1.\n", port);
    }

    tcflush(fd, TCIOFLUSH);
    g_uart_fd = fd;
    return true;
}

static void flush_rx() { tcflush(g_uart_fd, TCIFLUSH); }

static bool uart_write(const uint8_t* data, size_t len) {
    while (len > 0) {
        const ssize_t n = write(g_uart_fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("    Write to the serial port failed: %s\n", strerror(errno));
            return false;
        }
        data += n;
        len  -= size_t(n);
    }
    return tcdrain(g_uart_fd) == 0;  // wait until the bytes are really on the wire
}

// Reads bytes until max_len, or until the line goes quiet. Returns the count.
static int read_reply(uint8_t* buf, int max_len, int first_byte_timeout_ms) {
    int n = 0;
    int timeout = first_byte_timeout_ms;
    pollfd pfd{g_uart_fd, POLLIN, 0};
    while (n < max_len && poll(&pfd, 1, timeout) > 0) {
        const ssize_t got = read(g_uart_fd, buf + n, size_t(max_len - n));
        if (got <= 0) break;
        n += int(got);
        timeout = GAP_TIMEOUT_MS;
    }
    return n;
}

// ------------------------------------------------------------------ E32

static void print_hex(const uint8_t* buf, int n) {
    for (int i = 0; i < n; i++) printf(" %02X", buf[i]);
}

// Sets M0/M1 and waits for the module to finish switching mode.
static bool set_mode(bool m0, bool m1) {
    gpio_v2_line_values v{};
    v.mask = (1ULL << IDX_M0) | (1ULL << IDX_M1);
    v.bits = (uint64_t(m0) << IDX_M0) | (uint64_t(m1) << IDX_M1);
    if (ioctl(g_line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) {
        printf("    Warning: can't set M0/M1: %s\n", strerror(errno));
    } else {
        g_m0 = m0;
        g_m1 = m1;
    }
    sleep_ms(50);  // give the module time to see the change and pull AUX low
    const bool ok = wait_aux(AUX_TIMEOUT_MS);
    flush_rx();
    return ok;
}

// Sends "cmd cmd cmd" and reads the reply into buf.
static int send_command(uint8_t cmd, uint8_t* buf, int max_len) {
    const uint8_t frame[3] = {cmd, cmd, cmd};
    flush_rx();
    if (!uart_write(frame, sizeof frame)) return 0;
    return read_reply(buf, max_len, REPLY_TIMEOUT_MS);
}

// Decodes the 6-byte answer to C1 C1 C1. Returns false if something will
// stop node_a.c / node_b_pi3 from working.
static bool print_settings(const uint8_t r[6]) {
    static const char* const parity_name[] = {"8N1", "8O1", "8E1", "8N1"};
    static const unsigned baud_value[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    static const char* const air_name[] = {"0.3k", "1.2k", "2.4k", "4.8k", "9.6k", "19.2k", "19.2k", "19.2k"};

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
    printf("    Address, air rate, channel and FEC must match the Pico's module.\n");

    return uart_ok && !fixed;
}

// ------------------------------------------------------------------ Linux serial setup

static std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// Lists the other programs that have `dev` open, as " name(pid)". Without root,
// programs of other users (a getty runs as root) can't be seen.
static std::string port_users(const std::string& dev) {
    std::string users;
    DIR* proc = opendir("/proc");
    if (!proc) return users;

    const pid_t self = getpid();
    while (const dirent* p = readdir(proc)) {
        const pid_t pid = atoi(p->d_name);  // 0 for entries that aren't a process
        if (pid <= 0 || pid == self) continue;

        const std::string fd_dir = std::string("/proc/") + p->d_name + "/fd";
        DIR* fds = opendir(fd_dir.c_str());
        if (!fds) continue;  // process gone, or owned by another user
        while (const dirent* f = readdir(fds)) {
            char target[PATH_MAX];
            const ssize_t len = readlink((fd_dir + "/" + f->d_name).c_str(), target, sizeof target);
            if (len > 0 && std::string(target, size_t(len)) == dev) {
                users += " " + read_line(std::string("/proc/") + p->d_name + "/comm") + "(" + p->d_name + ")";
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return users;
}

static bool check_serial_port(const char* port) {
    printf("\n[0] Serial port %s\n", port);

    char* real = realpath(port, nullptr);
    if (!real) {
        printf("    FAIL: %s doesn't exist. Turn on the serial hardware with raspi-config\n"
               "    (see the README) and reboot.\n", port);
        return false;
    }
    const std::string dev = real;
    free(real);
    const std::string name = dev.substr(dev.rfind('/') + 1);
    bool ok = true;

    if (name == "ttyAMA0") {
        printf("    OK: %s -> %s, the main UART (PL011).\n", port, dev.c_str());
    } else if (name == "ttyS0") {
        printf("    Note: %s -> %s, the mini UART. Its baud rate follows the CPU clock,\n"
               "    so it needs enable_uart=1. dtoverlay=disable-bt in config.txt moves\n"
               "    serial0 to the main UART, which is more reliable.\n", port, dev.c_str());
    } else {
        printf("    %s -> %s\n", port, dev.c_str());
    }

    // A login console on the port reads the E32's bytes and sends boot text to it.
    const std::string cmdline = read_line("/proc/cmdline");
    bool console = false;
    for (const std::string& c : {std::string("console=serial0"), "console=" + name}) {
        if (cmdline.find(c) != std::string::npos) {
            printf("    FAIL: /proc/cmdline has %s, so a login console uses the port.\n", c.c_str());
            console = true;
        }
    }
    if (console) {
        printf("    raspi-config -> Interface Options -> Serial Port -> login shell: No.\n");
        ok = false;
    } else {
        printf("    OK: no login console on the port.\n");
    }

    const std::string users = port_users(dev);
    if (!users.empty()) {
        printf("    FAIL: other programs have the port open:%s\n"
               "    They take the bytes coming from the E32. Stop them first.\n", users.c_str());
        ok = false;
    } else {
        printf("    OK: no other program has the port open%s.\n",
               geteuid() == 0 ? "" : " (run with sudo to also see other users' programs)");
    }
    return ok;
}

// ------------------------------------------------------------------ checks

// Runs steps [0]-[3] and prints a summary. Returns true if everything passed.
static bool run_checks(const char* port) {
    uint8_t r[16];
    int n;

    printf("\n========== E32 check (Pi 3) ==========\n");

    const bool port_ok = check_serial_port(port);

    // [1] AUX wiring. With a pull-down, AUX only reads high if the module drives it.
    printf("\n[1] AUX on GPIO%u (pin 22)\n", PIN_AUX);
    set_aux_pull(false);
    set_mode(false, false);
    sleep_ms(10);
    const bool aux_ok = wait_aux(AUX_TIMEOUT_MS);
    if (aux_ok) {
        printf("    OK: the module drives AUX high.\n");
    } else {
        printf("    FAIL: AUX stays low. GPIO25 isn't connected to AUX, the module has no\n"
               "    power, or AUX is set to open-drain. Using a pull-up from here on.\n");
        set_aux_pull(true);
    }

    // [2] Mode 3: read settings and model.
    printf("\n[2] Mode 3 (M0=1, M1=1): reading the module settings\n");
    if (!set_mode(true, true)) printf("    Warning: AUX didn't go high after switching mode.\n");

    n = send_command(0xC1, r, 6);
    printf("    C1 C1 C1 ->");
    print_hex(r, n);
    printf("  (%d bytes)\n", n);

    const bool settings_ok = (n == 6) && (r[0] == 0xC0 || r[0] == 0xC2);
    bool config_ok = false;
    if (settings_ok) {
        config_ok = print_settings(r);
    } else if (n == 0) {
        printf("    FAIL: no answer. The Pi isn't talking to its E32. Check:\n"
               "      - pin 8 (TXD) -> E32 RXD and pin 10 (RXD) <- E32 TXD (try swapping them)\n"
               "      - pin 16 -> M0 and pin 18 -> M1\n"
               "      - E32 VCC, and GND shared with the Pi\n"
               "      - any FAIL in step [0]\n");
    } else {
        printf("    FAIL: unexpected answer. Usually a wiring or baud-rate problem (see step [0]).\n");
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
    if (!set_mode(false, false)) printf("    Warning: AUX didn't go high after switching mode.\n");

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
               "    check the wires on pins 16 and 18.\n");
    } else {
        printf("    Unclear: these bytes may be radio traffic from the Pico (test_pico\n"
               "    sends every 3 s). Type r + Enter to run the checks again.\n");
    }

    printf("\n---------- Summary ----------\n");
    printf("  Serial port     : %s\n", port_ok ? "OK" : "FAIL");
    printf("  AUX wire        : %s\n", aux_ok ? "OK" : "FAIL");
    printf("  Pi <-> E32      : %s\n", settings_ok ? "OK" : "FAIL");
    printf("  Settings        : %s\n", !settings_ok ? "unknown" : config_ok ? "OK" : "FAIL");
    printf("  M0/M1 wires     : %s\n", !settings_ok ? "unknown" : mode_ok ? "OK" : "FAIL");
    printf("-----------------------------\n");

    return port_ok && aux_ok && settings_ok && config_ok && mode_ok;
}

static void print_radio_banner() {
    printf("\n[4] Radio test: answering every 4-byte number with number + 1000, like node_b_pi3.\n");
    printf("    On the Pico run test_pico (or node_a). Every byte that arrives is printed here.\n"
           "    Type r + Enter to run the checks again, Ctrl+C to quit.\n\n");
}

static void print_time() {
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count();
    printf("[%5lld.%lld s]", ms / 1000, (ms % 1000) / 100);
}

// Handles one burst of bytes from the E32 during the radio test.
static void handle_rx() {
    uint8_t rx[64];
    const int n = read_reply(rx, sizeof rx, 0);
    if (n == 0) return;

    print_time();
    printf(" RX (%d bytes):", n);
    print_hex(rx, n);

    if (n == 4) {
        int32_t value;
        memcpy(&value, rx, sizeof value);
        printf("  = %ld\n", (long)value);

        // Answer like node_b_pi3: wait until the module is idle, then send value + 1000.
        if (!wait_aux(AUX_TIMEOUT_MS)) {
            print_time();
            printf(" Warning: AUX low for %d ms before sending.\n", AUX_TIMEOUT_MS);
        }
        sleep_ms(20);
        const int32_t reply = int32_t(uint32_t(value) + 1000u);  // wraps instead of overflowing
        uint8_t bytes[sizeof reply];
        memcpy(bytes, &reply, sizeof bytes);
        uart_write(bytes, sizeof bytes);

        print_time();
        printf(" TX %ld ->", (long)reply);
        print_hex(bytes, sizeof bytes);
        printf("\n");
    } else if (n == 3 && rx[0] == 0xC1 && rx[1] == 0xC1 && rx[2] == 0xC1) {
        printf("  <-- the Pico's step [3] sent over the air, so Pico -> Pi works!\n");
    } else {
        printf("  (not a 4-byte number, no answer)\n");
    }
}

static void usage(const char* prog) {
    printf("Usage: %s [--checks-only] [--port DEVICE]\n"
           "  -c, --checks-only  run steps [0]-[3] and exit (exit code 0 = all OK)\n"
           "  -p, --port DEVICE  serial port wired to the E32 (default %s)\n",
           prog, DEFAULT_PORT);
}

int main(int argc, char** argv) {
    const char* port = DEFAULT_PORT;
    bool checks_only = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-c" || arg == "--checks-only") {
            checks_only = true;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = argv[++i];
        } else {
            usage(argv[0]);
            return (arg == "-h" || arg == "--help") ? 0 : 2;
        }
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);  // print each line at once, even through "| tee log.txt"

    if (!gpio_open()) return 1;
    if (!uart_open(port)) {
        check_serial_port(port);  // explain why, if it can
        close(g_line_fd);
        return 1;
    }

    const bool all_ok = run_checks(port);
    if (checks_only) {
        close(g_uart_fd);
        close(g_line_fd);
        return all_ok ? 0 : 1;
    }

    print_radio_banner();

    pollfd fds[2] = {{g_uart_fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    nfds_t nfds = 2;

    while (true) {
        if (poll(fds, nfds, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (nfds == 2 && fds[1].revents) {
            char line[64];
            const ssize_t got = read(STDIN_FILENO, line, sizeof line);
            if (got <= 0) {
                nfds = 1;  // no keyboard (e.g. started from a script): stop watching stdin
            } else if (line[0] == 'r' || line[0] == 'R') {
                run_checks(port);
                print_radio_banner();
                continue;
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("The serial port reported an error. Stopping.\n");
            break;
        }
        if (fds[0].revents & POLLIN) handle_rx();
    }

    close(g_uart_fd);
    close(g_line_fd);
    return 1;
}
