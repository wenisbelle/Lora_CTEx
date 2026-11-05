#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

constexpr const char* GPIO_CHIP_PATH = "/dev/gpiochip0";
constexpr const char* SERIAL_DEVICE  = "/dev/serial0";

constexpr uint32_t PIN_M0  = 23; // Physical Pin 16
constexpr uint32_t PIN_M1  = 24; // Physical Pin 18
constexpr uint32_t PIN_AUX = 25; // Physical Pin 22

int main() {
    int chip_fd = open(GPIO_CHIP_PATH, O_RDONLY);
    if (chip_fd < 0) { perror("open gpiochip"); return 1; }

    struct gpio_v2_line_request req{};
    std::strncpy(req.consumer, "e32_diag", sizeof(req.consumer) - 1);
    req.offsets[0] = PIN_M0;
    req.offsets[1] = PIN_M1;
    req.offsets[2] = PIN_AUX;
    req.num_lines  = 3;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

    // Set M0 = HIGH (1), M1 = HIGH (1) -> Mode 3 (Config/Sleep Mode)
    req.config.num_attrs = 2;
    req.config.attrs[0].mask = (1ULL << 0) | (1ULL << 1);
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = (1ULL << 0) | (1ULL << 1); // Both HIGH!

    req.config.attrs[1].mask = (1ULL << 2);
    req.config.attrs[1].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
    req.config.attrs[1].attr.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        perror("ioctl get line"); return 1;
    }
    close(chip_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int uart_fd = open(SERIAL_DEVICE, O_RDWR | O_NOCTTY);
    if (uart_fd < 0) { perror("open serial"); return 1; }

    struct termios tty{};
    tcgetattr(uart_fd, &tty);
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 10; // 1.0 second read timeout
    tcflush(uart_fd, TCIOFLUSH);
    tcsetattr(uart_fd, TCSANOW, &tty);

    // Send 0xC1 0xC1 0xC1 to query E32 internal parameters
    uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
    write(uart_fd, cmd, 3);
    tcdrain(uart_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint8_t resp[6] = {0};
    ssize_t n = read(uart_fd, resp, 6);

    if (n == 6 && (resp[0] == 0xC0 || resp[0] == 0xC2)) {
        std::cout << "✅ SUCCESS! Pi 3 UART and E32 wiring are 100% working.\n";
        std::cout << "Raw Config Bytes: ";
        for (int i = 0; i < 6; ++i) {
            std::cout << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)resp[i] << " ";
        }
        std::cout << std::dec << "\n";
        std::cout << "  Address : 0x" << std::hex << ((resp[1] << 8) | resp[2]) << std::dec << "\n";
        std::cout << "  Channel : 0x" << std::hex << (int)resp[4] << std::dec << " (" << (410 + resp[4]) << " MHz)\n";
        std::cout << "  Fixed Mode: " << ((resp[5] & 0x80) ? "ENABLED (Requires 3-byte header!)" : "DISABLED (Transparent Mode)") << "\n";
    } else {
        std::cout << "❌ FAILED! Read " << n << " bytes from E32.\n";
        std::cout << "Check: 1) Swap TXD/RXD wires on Pins 8 & 10. 2) Verify M0/M1 on Physical Pins 16 & 18. 3) Verify /dev/serial0.\n";
    }

    close(uart_fd);
    close(req.fd);
    return 0;
}
