#include <iostream>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <gpiod.h>

constexpr const char* GPIO_CHIP_PATH = "/dev/gpiochip0";
constexpr const char* SERIAL_DEVICE  = "/dev/serial0";

constexpr unsigned int PIN_M0  = 23;
constexpr unsigned int PIN_M1  = 24;
constexpr unsigned int PIN_AUX = 25;

void wait_aux(gpiod_line* aux_line) {
    while (gpiod_line_get_value(aux_line) == 0) {
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

    // 8N1, raw binary mode (critical so Linux doesn't mangle 0x0A / 0x0D bytes in int32_t)
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;

    // Block until 4 bytes are received or 0.5s inter-byte timeout
    tty.c_cc[VMIN]  = 4;
    tty.c_cc[VTIME] = 5;

    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

int main() {
    gpiod_chip* chip = gpiod_chip_open(GPIO_CHIP_PATH);
    if (!chip) {
        std::cerr << "Failed to open " << GPIO_CHIP_PATH << "\n";
        return 1;
    }

    gpiod_line* m0_line  = gpiod_chip_get_line(chip, PIN_M0);
    gpiod_line* m1_line  = gpiod_chip_get_line(chip, PIN_M1);
    gpiod_line* aux_line = gpiod_chip_get_line(chip, PIN_AUX);

    // Drive M0=0, M1=0 for Mode 0 (Transparent)
    gpiod_line_request_output(m0_line, "e32_lora", 0);
    gpiod_line_request_output(m1_line, "e32_lora", 0);
    gpiod_line_request_input(aux_line, "e32_lora");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wait_aux(aux_line);

    int uart_fd = init_uart(SERIAL_DEVICE);
    if (uart_fd < 0) {
        std::cerr << "Failed to open UART " << SERIAL_DEVICE << "\n";
        return 1;
    }

    std::cout << "--- Pi 3 C++ Node B Ready on " << SERIAL_DEVICE << " ---\n";

    int32_t rx_num = 0;
    while (true) {
        ssize_t n = read(uart_fd, &rx_num, sizeof(rx_num));
        if (n == sizeof(rx_num)) {
            std::cout << "\n[Pi 3 Node B] Received: " << rx_num << "\n";

            wait_aux(aux_line);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            int32_t tx_num = rx_num + 1000;
            std::cout << "[Pi 3 Node B] Sending return message: " << tx_num << "\n";

            write(uart_fd, &tx_num, sizeof(tx_num));
            tcdrain(uart_fd);
            wait_aux(aux_line);
        }
    }

    close(uart_fd);
    gpiod_chip_close(chip);
    return 0;
}