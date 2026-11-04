#include <iostream>
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

// BCM GPIO Pin Numbers (Physical Pins 16, 18, 22)
constexpr uint32_t PIN_M0  = 23;
constexpr uint32_t PIN_M1  = 24;
constexpr uint32_t PIN_AUX = 25;

// Request M0 and M1 as outputs driven LOW (0), and AUX as an input with pull-up
int init_e32_gpio(const char* chip_path) {
    int chip_fd = open(chip_path, O_RDONLY);
    if (chip_fd < 0) {
        perror("Failed to open gpiochip");
        return -1;
    }

    struct gpio_v2_line_request req{};
    std::strncpy(req.consumer, "e32_lora_node_b", sizeof(req.consumer) - 1);

    req.offsets[0] = PIN_M0;
    req.offsets[1] = PIN_M1;
    req.offsets[2] = PIN_AUX;
    req.num_lines  = 3;

    // Default configuration: Output (applies to M0 and M1)
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

    // Attribute 0: Drive M0 (bit 0) and M1 (bit 1) LOW (0) for Mode 0 (Transparent)
    req.config.num_attrs = 2;
    req.config.attrs[0].mask = (1ULL << 0) | (1ULL << 1);
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0; // Both M0=0, M1=0

    // Attribute 1: Override AUX (index 2, bit 2) to Input with Pull-Up
    req.config.attrs[1].mask = (1ULL << 2);
    req.config.attrs[1].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
    req.config.attrs[1].attr.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        perror("Failed to request GPIO lines");
        close(chip_fd);
        return -1;
    }

    close(chip_fd); // Line request fd remains active after closing chip_fd
    return req.fd;
}

// Read the AUX pin state (index 2 in our request)
int read_aux(int line_fd) {
    struct gpio_v2_line_values vals{};
    vals.mask = (1ULL << 2); // Bit 2 corresponds to PIN_AUX
    if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
        return 0;
    }
    return (vals.bits & (1ULL << 2)) ? 1 : 0;
}

void wait_aux(int line_fd) {
    while (read_aux(line_fd) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

int init_uart(const char* dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;

    struct termios tty{};
    tcgetattr(fd, &tty);

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);

    // Raw binary 8N1 mode so 0x0A/0x0D bytes inside int32_t aren't altered
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;

    // Wait for 4 bytes (or 0.5s inter-byte timeout)
    tty.c_cc[VMIN]  = 4;
    tty.c_cc[VTIME] = 5;

    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

int main() {
    int gpio_fd = init_e32_gpio(GPIO_CHIP_PATH);
    if (gpio_fd < 0) {
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wait_aux(gpio_fd);

    int uart_fd = init_uart(SERIAL_DEVICE);
    if (uart_fd < 0) {
        std::cerr << "Failed to open UART " << SERIAL_DEVICE << "\n";
        close(gpio_fd);
        return 1;
    }

    std::cout << "--- Pi 3 C++ Node B Ready on " << SERIAL_DEVICE << " ---\n";

    int32_t rx_num = 0;
    while (true) {
        ssize_t n = read(uart_fd, &rx_num, sizeof(rx_num));
        if (n == sizeof(rx_num)) {
            std::cout << "\n[Pi 3 Node B] Received from Pico: " << rx_num << "\n";

            wait_aux(gpio_fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            int32_t tx_num = rx_num + 1000;
            std::cout << "[Pi 3 Node B] Sending return message: " << tx_num << "\n";

            write(uart_fd, &tx_num, sizeof(tx_num));
            tcdrain(uart_fd);
            wait_aux(gpio_fd);
        }
    }

    close(uart_fd);
    close(gpio_fd);
    return 0;
}