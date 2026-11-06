# E32 LoRa Ping-Pong: Pico ↔ Raspberry Pi 3

A Raspberry Pi Pico and a Raspberry Pi 3 talk to each other through two E32 LoRa modules.
Both modules run in transparent mode (mode 0) at 9600 8N1. Node A sends a 4-byte number,
and Node B answers with the same number + 1000.

| File                              | Runs on | What it does                                                    |
|-----------------------------------|---------|-----------------------------------------------------------------|
| `node_a.c`                        | Pico    | **Node A**: sends a number every 2 s and waits for the reply.   |
| `node_b.c`                        | Pico    | **Node B** on a second Pico: adds 1000 and sends it back.       |
| `Transmitter/node_b_pi3.cpp`      | Pi 3    | **Node B** on the Pi: same as `node_b.c`.                        |
| `test_pico.c`                     | Pico    | Diagnostic: wiring, module settings, radio test. [Details](#diagnostics-test_pico-and-test_pi3) |
| `Transmitter/test_pi3.cpp`        | Pi 3    | The same diagnostic for the Pi, plus the Pi's serial port setup. |
| `Transmitter/test_connection.cpp` | Pi 3    | Quick check: reads the Pi module's settings once.               |

`notes.md` has the debugging notes on why the two sides might not talk.

---

## Raspberry Pi Pico

### Wiring

| E32 pin | Pico pin        |
|---------|-----------------|
| RXD     | GP0 (UART0 TX)  |
| TXD     | GP1 (UART0 RX)  |
| M0      | GP2             |
| M1      | GP3             |
| AUX     | GP4             |
| VCC     | VSYS (preferred) or 3V3 |
| GND     | GND             |

When the module transmits it draws a burst of current, so 3V3 is marginal. See `notes.md`.

### Build

Requirements: `cmake`, `make` and the ARM toolchain:

```bash
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

The build uses the Pico SDK in `pico-sdk/` inside this folder. The folder is in
`.gitignore`, so a fresh clone needs to download it once (version 2.3.1 is the one tested):

```bash
git clone -b 2.3.1 --depth 1 https://github.com/raspberrypi/pico-sdk.git
git -C pico-sdk submodule update --init lib/tinyusb   # needed for USB printf
```

`CMakeLists.txt` finds it automatically, so you don't need `export PICO_SDK_PATH`. (To use
another copy, pass `-DPICO_SDK_PATH=/path/to/sdk` to `cmake` or set the environment variable.)

```bash
mkdir -p build && cd build
cmake ..
make
```

You should now have `lora_node_a.uf2`, `lora_node_b.uf2` and `test_pico.uf2` inside `build/`.
The first build also downloads and builds `picotool`, so it needs internet.

### Flash

1. Hold the **BOOTSEL** button and plug the Pico into USB.
2. A drive called `RPI-RP2` appears.
3. Copy the `.uf2` file onto it:
   ```bash
   cp lora_node_a.uf2 /media/$USER/RPI-RP2/     # Node A
   cp test_pico.uf2   /media/$USER/RPI-RP2/     # or the diagnostic
   ```
4. The Pico reboots and runs the firmware.

### See the debug output

Debug `printf` goes over USB, so the Pico shows up as a serial port,
usually `/dev/ttyACM0` (a second Pico will be `/dev/ttyACM1`).

```bash
ls /dev/ttyACM*
```

**screen**
```bash
screen /dev/ttyACM0 115200
```
Exit with `Ctrl+A` then `K`, then `Y`.

**minicom**
```bash
minicom -D /dev/ttyACM0 -b 115200
```
Exit with `Ctrl+A` then `X`.

The baud rate doesn't matter for USB serial; any value works.

If you get *Permission denied*, add yourself to the `dialout` group and log out/in:
```bash
sudo usermod -aG dialout $USER
```

`node_a` and `node_b` wait 5 s at startup so you have time to open the terminal. If you open
it too late you miss the first lines; unplug and replug the Pico to see them again.
`test_pico` waits until a terminal is open (up to 15 s), so it doesn't lose its report.

Expected output on Node A:
```
--- Node A (Initiator) Initializing ---
E32 Ready. Starting Ping-Pong loop...

[Node A] Sending number: 1
[Node A] Received return message from Node B: 1001
```

---

## Raspberry Pi 3

The programs in `Transmitter/` run on the Pi itself. They use `/dev/serial0` for the E32's
UART and the Linux GPIO character device (`/dev/gpiochip0`) for M0, M1 and AUX.

Requirements: Raspberry Pi OS Bullseye or newer (Linux 5.10+ for the GPIO v2 API).

### Wiring

TX and RX cross over:

| E32 pin | Pi pin                 |
|---------|------------------------|
| RXD     | GPIO14 / TXD (pin 8)   |
| TXD     | GPIO15 / RXD (pin 10)  |
| M0      | GPIO23 (pin 16)        |
| M1      | GPIO24 (pin 18)        |
| AUX     | GPIO25 (pin 22)        |
| VCC     | 5V (pin 2 or 4)        |
| GND     | GND (pin 6)            |

The Pi and the E32 must share a ground.

### Configure the serial port (once)

By default the Pi 3 is not ready for this:

- `/dev/serial0` is the *mini UART*, whose baud rate follows the CPU clock, because
  Bluetooth uses the main UART.
- A Linux login console often runs on `/dev/serial0`. It reads the bytes coming from the E32
  and sends boot messages to it, which the E32 then transmits over the air.

1. Turn off the serial console and turn on the serial hardware:
   ```bash
   sudo raspi-config
   ```
   *Interface Options → Serial Port*:
   - "Login shell accessible over serial?" → **No**
   - "Serial port hardware enabled?" → **Yes**

2. Give the GPIO header the main UART. Edit `/boot/firmware/config.txt`
   (`/boot/config.txt` on Bullseye and older) and make sure it has these lines:
   ```
   enable_uart=1
   dtoverlay=disable-bt
   ```
   Then stop the Bluetooth modem service, which would otherwise use that UART:
   ```bash
   sudo systemctl disable hciuart
   ```
   This turns Bluetooth off. If you need Bluetooth, skip this step: the mini UART works
   with `enable_uart=1`, but it's less reliable.

3. Let your user open the serial port and the GPIO chip. The default user is usually
   already in both groups.
   ```bash
   sudo usermod -aG dialout,gpio $USER
   ```

4. Reboot, then check:
   ```bash
   ls -l /dev/serial0          # should point to ttyAMA0
   cat /proc/cmdline           # must NOT contain console=serial0 / ttyS0 / ttyAMA0
   sudo fuser -v /dev/serial0  # nothing else should have the port open
   groups                      # should list dialout and gpio
   ```
   `test_pi3` checks the first three for you (step [0]).

### Build

The `Transmitter/` folder is a separate CMake project for the Pi; it doesn't need the Pico
SDK. Clone the repository on the Pi (or copy just `Transmitter/`) and build there:

```bash
sudo apt install cmake g++
cd Transmitter
mkdir -p build && cd build
cmake ..
make
```

This gives you three programs in `Transmitter/build/`: `node_b_pi3`, `test_pi3` and
`test_connection`.

### Run

```bash
./node_b_pi3
```

With Node A running on the Pico you should see:
```
--- Pi 3 C++ Node B Ready on /dev/serial0 ---

[Pi 3 Node B] Received from Pico: 1
[Pi 3 Node B] Sending return message: 1001
```

Only one program can hold the GPIO pins at a time. Stop `node_b_pi3` (`Ctrl+C`) before
running `test_pi3` or `test_connection`; otherwise they report *Device or resource busy*.

---

## Diagnostics: `test_pico` and `test_pi3`

`node_a` and `node_b_pi3` can't tell you *why* nothing arrives. Both use a pull-up on AUX
and wait for it to go high, so they look healthy even with **no E32 connected at all**.
The two diagnostics check each part of the chain, one step at a time:

- `test_pico` runs on the Pico.
- `test_pi3` is its Pi version.

Both use the same step numbers, so you can compare the two outputs side by side.

### What `test_pico` does

Flash `test_pico.uf2` and open the USB terminal. The program waits until the terminal is
open, then runs:

| Step | What it does | What a failure means |
|------|--------------|----------------------|
| **[1] AUX on GP4** | Switches GP4 to a **pull-down** and checks that AUX still reads high. With a pull-down, AUX only reads high if the module really drives it. | GP4 isn't connected to AUX, the module has no power, or AUX is set to open-drain. The test switches to a pull-up and carries on. |
| **[2] Mode 3** (M0=1, M1=1) | Sends `C1 C1 C1` and reads the 6-byte settings answer. It decodes address, UART, air rate, channel, transmission mode, IO drive, FEC and TX power, then sends `C3 C3 C3` to read the model (frequency band). | No answer: the Pico isn't talking to its E32 (TX/RX swapped, M0/M1, VCC/GND). It also flags settings that break the ping-pong: a UART other than 9600 8N1, or *fixed transmission* mode, which swallows the first 3 bytes of every message. |
| **[3] Mode 0** (M0=0, M1=0) | Sends `C1 C1 C1` again. In mode 0 the module transmits those bytes over the air instead of answering, so it **must stay silent**. | It still answers with the settings: the module is stuck in mode 3, so M0/M1 aren't reaching it (the E32 pulls them high when they're unconnected). |
| **Summary** | One OK/FAIL line each for AUX, Pico ↔ E32, settings and M0/M1. | |
| **[4] Radio test** | Every 3 s sends a 4-byte number like `node_a` (`01 00 00 00`, `02 00 00 00`, …) and prints in hex every byte that arrives. A reply equal to *sent + 1000* is marked `<-- reply from node_b_pi3, link works!`. | See [Reading the radio test](#reading-the-radio-test). |

Press `r` in the terminal to run steps [1]–[3] again (useful after fixing a wire).

The settings are compared against the factory default, `C0 00 00 1A 17 44`:
address `0x0000`, 9600 8N1, 2.4 kbps air rate, channel `0x17` (433 MHz), transparent mode.
The two modules must agree on **address, air rate, channel, FEC and model byte**.

### What `test_pi3` does

It runs the same steps [1]–[4] through the Pi's pins. It also adds one Pi-only step at the
start:

- **[0] Serial port**: shows where `/dev/serial0` points (`ttyAMA0` = main UART, good;
  `ttyS0` = mini UART). It fails if a login console is on the port (`/proc/cmdline`), or if
  another program has the port open (for example a getty, or a forgotten `node_b_pi3`).
  Run it with `sudo` to also see programs owned by other users.

In step [4] the Pi doesn't send numbers. Instead it **answers** every 4-byte number with
*number + 1000*, exactly like `node_b_pi3`, and prints every byte it receives. So
`test_pico` on one side and `test_pi3` on the other test the whole round trip, with a hex
dump on both ends.

```bash
cd Transmitter/build
./test_pi3                  # checks [0]-[3], then radio test [4]; r + Enter re-runs the checks
./test_pi3 --checks-only    # checks only; exit code 0 = everything OK
./test_pi3 --port /dev/ttyAMA0
```

A healthy Pi with a factory-default module ends its checks with:
```
---------- Summary ----------
  Serial port     : OK
  AUX wire        : OK
  Pi <-> E32      : OK
  Settings        : OK
  M0/M1 wires     : OK
-----------------------------
```

`test_connection` is a smaller, older check. It only does what step [2] does, without the
decoding of every field.

### Running both together

1. Flash `test_pico.uf2` and open its USB terminal.
2. On the Pi, stop `node_b_pi3` and run `./test_pi3`.
3. Make sure both summaries are all OK, and that both step [2] outputs show the same
   address, air rate, channel, FEC and model byte.
4. Watch step [4]. When the link works, the two sides show:

   Pico (`test_pico`):
   ```
   [   21.0 s] TX 4 -> 04 00 00 00
   [   21.6 s] RX (4 bytes): EC 03 00 00  = 1004  <-- reply from node_b_pi3, link works!
   ```
   Pi (`test_pi3`):
   ```
   [   12.3 s] RX (4 bytes): 04 00 00 00  = 4
   [   12.4 s] TX 1004 -> EC 03 00 00
   ```
   (The Pico's message names `node_b_pi3`, but `test_pi3` answers the same way.)

One side's step [3] sends `C1 C1 C1` over the air. If the other side is already in step [4],
it receives those 3 bytes, which proves that direction of the link works. `test_pi3` marks
them `<-- the Pico's step [3] sent over the air`. The reverse can also happen: one side's
step [3] picks up the other side's radio traffic and reports *Unclear*. Re-run the checks
with `r`.

### Reading the radio test

| What you see | What it means |
|--------------|---------------|
| Pi prints `RX … = N` and the Pico prints the `+1000` reply | The link works both ways. If `node_a`/`node_b_pi3` still fail, the problem is in those programs. |
| Pi prints `RX`, the Pico never gets a reply | Pico → Pi works, Pi → Pico doesn't. Check the Pi's TX wire (pin 8 → E32 RXD) and AUX. |
| Pi prints nothing | Pico → Pi doesn't work. Compare both step [2] outputs (channel, air rate, address, model), check the antennas, and keep the modules more than 1 m apart. |
| Random bytes instead of `04 00 00 00` | The UART speed is wrong. See step [0] and *Configure the serial port*. |

### Fixing the summary

| FAIL line | Fix |
|-----------|-----|
| Serial port (Pi only) | [Configure the serial port](#configure-the-serial-port-once), then reboot. |
| AUX wire | Check the AUX wire (GP4 / pin 22) and the module's power. |
| Pico ↔ E32 / Pi ↔ E32 | Swap TXD/RXD, check M0/M1, VCC and the shared GND. On the Pi, fix any step [0] failure first. |
| Settings | The module isn't 9600 8N1, or is in fixed transmission mode. Write the factory settings back: in mode 3, send `C0 00 00 1A 17 44`. These tools don't do that. |
| M0/M1 wires | Check the M0/M1 wires (GP2/GP3 on the Pico, pins 16/18 on the Pi). |
