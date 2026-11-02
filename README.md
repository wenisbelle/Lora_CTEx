# Pico + E32 LoRa Ping-Pong

Two firmwares for the Raspberry Pi Pico talking through an E32 LoRa module:

- **Node A** (`node_a.c`): sends a number every 2 s and waits for the reply.
- **Node B** (`node_b.c`): receives the number, adds 1000 and sends it back.

## Wiring (both nodes)

| E32 pin | Pico pin        |
|---------|-----------------|
| RXD     | GP0 (UART0 TX)  |
| TXD     | GP1 (UART0 RX)  |
| M0      | GP2             |
| M1      | GP3             |
| AUX     | GP4             |
| VCC     | 3V3 or VSYS     |
| GND     | GND             |

## Build

Requirements: `cmake`, `make` and the ARM toolchain (`gcc-arm-none-eabi`).

```bash
cd Lora
export PICO_SDK_PATH=$PWD/pico-sdk     # SDK included in this folder
mkdir -p build && cd build
cmake ..
make
```

You should now have `lora_node_a.uf2` and `lora_node_b.uf2` inside `build/`.

> If the build fails while setting up USB stdio, run
> `git -C ../pico-sdk submodule update --init` once and build again.

## Flash the Pico

1. Hold the **BOOTSEL** button and plug the Pico into USB.
2. A drive called `RPI-RP2` appears.
3. Copy the `.uf2` file onto it:
   ```bash
   cp lora_node_a.uf2 /media/$USER/RPI-RP2/     # first Pico
   cp lora_node_b.uf2 /media/$USER/RPI-RP2/     # second Pico
   ```
4. The Pico reboots and runs the firmware.

## See the debug output

Debug `printf` goes over USB, so the Pico shows up as a serial port,
usually `/dev/ttyACM0` (the second Pico will be `/dev/ttyACM1`).

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

The firmware waits 2.5 s at startup so you have time to open the terminal.
If you open it too late you miss the first lines. Unplug and replug the Pico to see them again.

Expected output on Node A:
```
--- Node A (Initiator) Initializing ---
E32 Ready. Starting Ping-Pong loop...

[Node A] Sending number: 1
[Node A] Received return message from Node B: 1001
```
