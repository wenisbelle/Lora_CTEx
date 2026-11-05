# Debug notes: Pico (Node A) and RPi3 (Node B) don't communicate

Date: 2026-11-04

## Symptom

- **Pico running `node_a.c`:** prints `Sending number: N` and `Timeout waiting for Node B reply` over and over. It looks fine on the USB serial port.
- **RPi3 running `Transmitter/node_b_pi3.cpp`:** prints `--- Pi 3 C++ Node B Ready on /dev/serial0 ---`, then waits and never receives anything.

## What was checked in the code

No protocol bug was found. Node A and the Pi program match on everything that matters:

- UART at 9600 8N1 on both sides.
- E32 in mode 0 (transparent, M0=M1=0) on both sides.
- Same 4-byte `int32_t` payload. Both CPUs are little-endian, so the byte order matches.
- Same AUX handling.
- `node_b_pi3.cpp` compiles cleanly with `-Wall -Wextra`.

So the cause is most likely the Pi's serial setup, the wiring, or the radio modules' settings, not the program logic.

## The current output doesn't prove the radios work

Both programs turn on the internal pull-up on AUX and wait for it to go high. If an E32 is unpowered or AUX isn't connected, the pin still reads high and the program carries on.

With **no E32 connected at all**, the output would be exactly what we see now:

- **Pico:** `Sending number: N`, then `Timeout waiting for Node B reply`, repeating.
- **Pi:** `Ready`, then `read()` waits forever.

So "seems to be working" only tells us the host programs run. It doesn't tell us either host is talking to its E32.

## Likely causes, most likely first

### 1. The Pi 3's serial port (top suspect)

On a Pi 3, `/dev/serial0` points to the mini UART (`ttyS0`) by default, because Bluetooth takes the main UART (`ttyAMA0`). Two things commonly break it:

- **The Linux serial console is on `serial0`.** A login prompt (getty) then reads the same port and takes the bytes coming from the E32. Boot and login text also gets sent to the E32, which transmits it over the air.
- **`enable_uart=1` is missing.** The mini UART's speed then follows the CPU clock, so 9600 isn't really 9600.

To fix it, first run:

```bash
sudo raspi-config   # Interface Options → Serial Port → login shell: NO, serial hardware: YES
```

Then add these lines to `/boot/firmware/config.txt` (`/boot/config.txt` on older OS versions):

```
enable_uart=1
dtoverlay=disable-bt
```

`disable-bt` gives GPIO14/15 the stable main UART. After adding it, also run:

```bash
sudo systemctl disable hciuart
```

Reboot, then check that it worked:

```bash
ls -l /dev/serial0        # should point to ttyAMA0 after disable-bt
cat /proc/cmdline         # must NOT contain console=serial0 / ttyS0 / ttyAMA0
sudo fuser -v /dev/serial0  # nothing else should have the port open
```

### 2. Wiring on the Pi

TX and RX must cross over:

| E32 pin | Pi pin                 |
|---------|------------------------|
| TXD     | GPIO15 / RXD (pin 10)  |
| RXD     | GPIO14 / TXD (pin 8)   |
| M0      | GPIO23 (pin 16)        |
| M1      | GPIO24 (pin 18)        |
| AUX     | GPIO25 (pin 22)        |
| GND     | any GND pin            |

The Pi and the E32 must share a ground.

### 3. The two modules have different settings

In transparent mode, the two E32s only hear each other if:

- They are the same frequency variant (433 vs 868/915).
- They share the same address, channel and air data rate.
- Neither is in "fixed transmission" mode. In fixed mode, the module treats the first 3 bytes of each message as an address and channel and swallows them. Node A's first message `01 00 00 00` would be eaten that way.

The factory default is `C0 00 00 1A 17 44`:

- Address `0x0000`
- UART 9600 8N1
- Air data rate 2.4 kbps
- Channel `0x17` (433 MHz)
- Transparent mode

A module that was configured before, or bought from a different batch, can differ.

**How to read a module's settings:**

1. Put the module in mode 3 (M0=1, M1=1).
2. Wait for AUX to go high.
3. Send `C1 C1 C1` at 9600 8N1.
4. The module answers with 6 bytes in the same format as the default above.

Doing this on each side also proves that the host is wired to its module.

### 4. Radio and power

- Both modules need antennas attached.
- Don't put the modules right next to each other. The 1 W T30D models can overload each other's receiver at under a metre.
- Transmitting draws a burst of current: about 120 mA on a T20D, much more on a T30D. Powering the E32 from the Pico's 3V3 pin is marginal. Prefer VSYS/5V on the Pico and the 5V pin on the Pi.

## Quick test to find which side is failing

Stop `node_b_pi3`, leave Node A running, and on the Pi run:

```bash
stty -F /dev/serial0 9600 raw -echo && hexdump -C /dev/serial0
```

Every ~3.5 s you should see `01 00 00 00`, then `02 00 00 00`, and so on.

| What you see   | What it means                                                   |
|----------------|-----------------------------------------------------------------|
| Clean bytes    | The radio link works; the problem is in the Pi program.         |
| Random garbage | The UART speed is wrong (cause 1).                              |
| Nothing        | Cause 1, 2, 3 or 4. Read the module settings on both sides to narrow it down. |

## Smaller code issues (not the main cause, but worth fixing)

- **No real check that the module is there.** The AUX waits (`e32_wait_aux()` in `node_a.c`, `wait_aux()` in `node_b_pi3.cpp`) have no timeout, and the pull-up hides a missing AUX wire. Reading the module settings at startup would be a real health check.
- **Serial setup isn't checked.** In `node_b_pi3.cpp` (`init_uart()`), the results of `tcgetattr`/`tcsetattr` aren't checked, and `CSTOPB` (2 stop bits) isn't explicitly cleared.
- **Short reads are dropped.** In `node_b_pi3.cpp` (main loop), a read that returns only 1–3 bytes is silently dropped. That can knock the 4-byte framing out of step for every message after it.

## Next steps

1. Fix the Pi serial port setup (cause 1) and check the wiring (cause 2).
2. Run the `hexdump` test above.
3. If nothing arrives, read both modules' settings (cause 3) and compare them.
4. Optional: add a `--check` mode to `node_a.c` and `node_b_pi3.cpp` that reads and prints the module settings. One run would then show whether each host can talk to its E32 and whether the two modules match.
