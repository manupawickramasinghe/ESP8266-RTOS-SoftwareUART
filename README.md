# ESP8266 RTOS Software UART

[![CI](https://github.com/manupawickramasinghe/ESP8266-RTOS-SoftwareUART/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/manupawickramasinghe/ESP8266-RTOS-SoftwareUART/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: ESP8266](https://img.shields.io/badge/platform-ESP8266-orange.svg)
![SDK: ESP8266_RTOS_SDK v3.4](https://img.shields.io/badge/SDK-ESP8266__RTOS__SDK%20v3.4-red.svg)

A bit-banged (software) UART for the **ESP8266** on Espressif's
[ESP8266_RTOS_SDK](https://github.com/espressif/ESP8266_RTOS_SDK) (FreeRTOS,
ESP-IDF style), plus an example application.

The ESP8266 has one full UART (UART0, used by the console and the
bootloader) and a transmit-only UART1. This library adds more serial
ports on ordinary GPIOs. Use them for GPS receivers, sensors, RS-485
transceivers and other low-speed serial peripherals.

## Contents

- [Features](#features)
- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Getting started](#getting-started)
- [Using the library](#using-the-library)
- [API reference](#api-reference)
- [Configuration](#configuration)
- [Limitations](#limitations)
- [Project layout](#project-layout)
- [Testing](#testing)
- [Continuous integration](#continuous-integration)
- [Contributing](#contributing)
- [Credits and license](#credits-and-license)

## Features

- Up to `SOFTUART_MAX_UARTS` independent ports (2 by default).
- 8 data bits, no parity, 1 stop bit (8N1), with optional extra idle
  time after each transmitted byte.
- Bit timing from the CPU cycle counter: every bit edge lands within one
  CPU cycle of its exact position at any baud rate, with no drift.
- Optional inverted line logic, for links that idle low.
- Interrupt-driven receive into a per-port ring buffer (64 bytes by default),
  with non-blocking and timed reads plus overflow detection.
- Blocking transmit of single bytes, C strings, byte buffers or
  `printf`-formatted text.
- Packaged as a reusable ESP-IDF component, configured through `menuconfig`.
- Builds with both the legacy GNU Make and the CMake (`idf.py`) build systems.

## How it works

```
 idle  start   D0   D1   D2   D3   D4   D5   D6   D7   stop  idle
 ────┐      ┌────┐    ┌──────────────┐    ┌────┐    ┌─────────────
     └──────┘    └────┘              └────┘    └────┘
      1 bit  ◄─────── 8 data bits, LSB first ───────► 1 bit
```

**Timing.** When a port is opened, the bit period is converted to CPU
cycles (`cpu_hz / baud`), keeping the remainder. Bit boundaries are
measured from the start of the frame on the CPU cycle counter (`CCOUNT`).
The remainder is carried from bit to bit, so even rates that are not a
whole number of microseconds (13.35 µs per bit at 74880 baud, 8.68 µs at
115200) do not drift.

**Receive.** The RX pin has an edge interrupt that fires on the falling
edge of the start bit (rising edge when inverted). The handler samples the
8 data bits in the middle of each bit period, 1.5 to 8.5 bit periods after
the edge. It pushes the byte into the port's ring buffer, waits for the
middle of the stop bit and re-arms the interrupt. `softuart_available()`
and `softuart_read()` drain the buffer from task context.

**Transmit.** `softuart_put()` drives the TX pin from the calling task and
returns after the stop bit plus any configured idle time.

The interrupt handler and the transmit loop run from IRAM and access the
GPIO registers directly, so a flash-cache miss cannot delay a sample or
an edge.

## Hardware

### Wiring

Example wiring between a NodeMCU / Wemos D1 mini and a 3.3 V USB-to-serial
adapter, using the pins from the example application:

```
   ESP8266                         USB-serial adapter (3.3 V)
 ┌───────────────┐               ┌──────────────────┐
 │ GPIO4 (D2) RX │◄──────────────┤ TX               │
 │ GPIO5 (D1) TX ├──────────────►│ RX               │
 │           GND ├───────────────┤ GND              │
 └───────────────┘               └──────────────────┘
```

> [!WARNING]
> ESP8266 GPIOs are 3.3 V. Use a level shifter or a voltage divider
> when connecting 5 V devices.

### Choosing pins

| GPIO | NodeMCU | Suitable for | Notes |
|---|---|---|---|
| 4, 5 | D2, D1 | RX / TX | Best choice: no boot or flash function |
| 12, 13, 14 | D6, D7, D5 | RX / TX | Free unless you use the HSPI peripheral |
| 0, 2 | D3, D4 | TX with care | Boot-strapping pins; must be high at reset |
| 15 | D8 | TX with care | Boot-strapping pin; must be low at reset |
| 1, 3 | TX, RX | Avoid | UART0: console and flashing |
| 6 – 11 | — | Never | Connected to the SPI flash |
| 16 | D0 | TX only | No GPIO interrupt, so it cannot receive |

## Getting started

### 1. Install the SDK and toolchain

Follow Espressif's
[ESP8266_RTOS_SDK get-started guide](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/get-started/index.html),
or on Linux:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b release/v3.4 --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
curl -LO https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz
tar -xzf xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz

export IDF_PATH=~/esp/ESP8266_RTOS_SDK
export PATH="$HOME/esp/xtensa-lx106-elf/bin:$PATH"
python3 -m pip install --user "setuptools<70" -r "$IDF_PATH/requirements.txt"
```

### 2. Build and flash the example

```sh
git clone https://github.com/manupawickramasinghe/ESP8266-RTOS-SoftwareUART.git
cd ESP8266-RTOS-SoftwareUART
```

With the CMake build system:

```sh
python3 $IDF_PATH/tools/idf.py build
python3 $IDF_PATH/tools/idf.py -p /dev/ttyUSB0 flash monitor
```

Or with the legacy GNU Make build system:

```sh
make defconfig            # first build only: creates sdkconfig from sdkconfig.defaults
make -j"$(nproc)"
make flash monitor ESPPORT=/dev/ttyUSB0
```

The console and monitor run at **74880 baud**, the ESP8266 boot ROM's
native rate. Prebuilt images for each tagged release are on the
[Releases](https://github.com/manupawickramasinghe/ESP8266-RTOS-SoftwareUART/releases)
page. To flash them, run
`esptool.py --chip esp8266 write_flash $(cat flash_args)` from the
extracted folder.

### 3. Try it

By default the example opens software UART 0 on RX = GPIO4, TX = GPIO5
at 9600 baud. To change the pins, baud rate or line inversion, run
`idf.py menuconfig` (or `make menuconfig`) and open
**Software UART example**.

Connect a USB-serial adapter as shown under [Wiring](#wiring) and open
it at 9600 8N1 (for example `picocom -b 9600 /dev/ttyUSB1`). You should
see:

```
Hello from Soft UART! Type something and it is echoed back.
[heartbeat 1]
```

Whatever you type is echoed back. Each completed line is also logged on
the ESP8266 console:

```
I (12345) softuart-example: received: hello
```

## Using the library

The library is the ESP-IDF component in `components/softuart`. Add it to
your project in either of these ways:

- Copy the `components/softuart` directory into your project's
  `components/` directory.
- Point your build at a checkout of this repository. In the project's
  top-level `CMakeLists.txt`, before `include($ENV{IDF_PATH}/...)`:

  ```cmake
  set(EXTRA_COMPONENT_DIRS /path/to/ESP8266-RTOS-SoftwareUART/components)
  ```

  For legacy make, in the project `Makefile`:

  ```make
  EXTRA_COMPONENT_DIRS := /path/to/ESP8266-RTOS-SoftwareUART/components
  ```

The `main` component can use it right away. In CMake builds, any other
component that includes `softuart.h` needs `REQUIRES softuart` (or
`PRIV_REQUIRES softuart`) in its `idf_component_register()`. Then:

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "softuart.h"

#define SENSOR_UART 0

void app_main(void)
{
    // Port 0: 9600 baud, RX on GPIO4, TX on GPIO5, normal (idle-high) logic
    if (!softuart_open(SENSOR_UART, 9600, 4, 5, false)) {
        printf("softuart_open failed\n");
        return;
    }

    softuart_puts(SENSOR_UART, "hello\r\n");

    while (true) {
        uint8_t buf[32];
        // Wait up to 100 ms for data; returns early once buf is full
        size_t n = softuart_read_bytes(SENSOR_UART, buf, sizeof(buf), 100);
        fwrite(buf, 1, n, stdout);

        if (softuart_rx_overflow(SENSOR_UART)) {
            printf("\nreceive buffer overflowed\n");
        }
    }
}
```

## API reference

All functions take the port index `uart_no`, from `0` to
`SOFTUART_MAX_UARTS - 1`.

| Function | Description |
|---|---|
| `bool softuart_open(uint8_t uart_no, uint32_t baudrate, uint32_t rx_pin, uint32_t tx_pin, bool invert)` | Configure the pins, install the RX interrupt and start receiving. Re-opening a port reconfigures it, and the receive buffer always starts empty. `invert = true` selects idle-low logic. Returns `false` for an invalid port, a baud rate outside 1 – 1,000,000, an RX pin outside GPIO0 – 15, a TX pin outside GPIO0 – 16, RX equal to TX, or pins already used by another port. |
| `bool softuart_close(uint8_t uart_no)` | Stop receiving and release the port. Other ports keep running. The GPIO ISR service is uninstalled only if softuart installed it and no port is left open, so an application's own GPIO interrupts are not affected. |
| `bool softuart_put(uint8_t uart_no, char c)` | Transmit one byte. Blocks for the length of the frame. |
| `bool softuart_puts(uint8_t uart_no, const char *s)` | Transmit a NUL-terminated string. |
| `bool softuart_putn(uint8_t uart_no, const uint8_t *s, size_t n)` | Transmit `n` bytes from a buffer. |
| `int softuart_printf(uint8_t uart_no, const char *format, ...)` | Format like `printf()` and transmit. Returns the number of bytes sent or `-1`. Output longer than 127 bytes is formatted in a temporary heap buffer. |
| `bool softuart_available(uint8_t uart_no)` | `true` if at least one received byte is waiting in the buffer. |
| `size_t softuart_rx_count(uint8_t uart_no)` | Number of received bytes waiting in the buffer. |
| `bool softuart_read_byte(uint8_t uart_no, uint8_t *byte)` | Take the next received byte without blocking. Returns `false` if the buffer is empty, so a received `0x00` is not mistaken for "no data". |
| `size_t softuart_read_bytes(uint8_t uart_no, void *buf, size_t len, uint32_t timeout_ms)` | Read up to `len` bytes, waiting up to `timeout_ms` for them (`0`: only what is buffered, `SOFTUART_WAIT_FOREVER`: until `len` bytes arrive or the port closes). Sleeps one RTOS tick at a time while waiting, so call it from a task. |
| `uint8_t softuart_read(uint8_t uart_no)` | Take the next received byte. Returns `0` if the buffer is empty, so call `softuart_available()` first or use `softuart_read_byte()`. |
| `bool softuart_rx_flush(uint8_t uart_no)` | Discard all buffered received bytes. |
| `bool softuart_rx_overflow(uint8_t uart_no)` | `true` if received bytes were dropped because the buffer was full since the previous call or since the port was opened. Clears the flag. |

The receive functions of one port are meant for one task: the ring
buffer has a single consumer. Use a mutex if several tasks share a port.

## Configuration

Run `idf.py menuconfig` (or `make menuconfig`) and open
**Component config → Software UART (softuart)**:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_SOFTUART_MAX_UARTS` | `2` | Number of software UART ports (1 – 8). The header exposes it as `SOFTUART_MAX_UARTS`. |
| `CONFIG_SOFTUART_RX_BUFFER_SIZE` | `64` | Receive ring-buffer size per port: 16, 32, 64, 128 or 256 bytes. One slot stays free, so a port holds up to size − 1 bytes. When the buffer is full, newer bytes are dropped. The header exposes it as `SOFTUART_MAX_RX_BUFF`. |

A compiler definition of `SOFTUART_MAX_UARTS` or `SOFTUART_MAX_RX_BUFF`
still overrides the menuconfig value. The buffer size must be a power of
two from 2 to 256, which is checked at compile time.

The example application has its own **Software UART example** menu with
the RX and TX GPIOs, the baud rate and line inversion.

Two more options tune transmission:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_SOFTUART_TX_EXTRA_IDLE_BITS` | `0` | Idle time after each transmitted stop bit, in bit times (0 – 10). Use `1` for receivers that need two stop bits. Versions before this option always left about 5 extra bit times; set `5` to reproduce that. |
| `CONFIG_SOFTUART_TX_CRITICAL_SECTION` | off | Disable interrupts while a frame is sent, so Wi-Fi or timer interrupts cannot stretch a bit. Other interrupts, including reception on other ports, then wait for up to one frame (about 1 ms at 9600 baud). |

The library logs configuration errors, such as an invalid pin or baud
rate passed to `softuart_open()`, through `ESP_LOGE` with the tag
`softuart`. It never logs from the interrupt handler.

## Limitations

A software UART uses the CPU to time every bit, and that has costs:

- **The CPU is busy for every byte.** Receiving a byte holds the CPU in the
  GPIO interrupt for about 9.5 bit times (≈ 1 ms at 9600 baud).
  Transmitting busy-waits in the calling task for the 10-bit frame plus
  any configured idle time.
- **In practice the link is half-duplex.** A byte arriving while
  `softuart_put()` is running interrupts the transmitter and distorts the
  outgoing bits. Two ports also cannot receive at the same moment.
- **Interrupt latency limits the speed.** The receiver measures from the
  moment its handler starts, which is a few microseconds after the
  start-bit edge. Wi-Fi and other interrupts can delay it further, or
  stretch a transmitted bit (see `CONFIG_SOFTUART_TX_CRITICAL_SECTION`).
  The timing arithmetic is verified in simulation up to 230400 baud.
  On hardware, 9600 baud is the proven use case (a GPS module, by the
  original authors), and margins shrink as the baud rate rises.
- **The CPU clock is read when a port opens.** Call `softuart_open()`
  again after changing the CPU frequency at run time.
- **Only 8N1.** There is no parity, no 2-stop-bit mode and no flow control.
- **Timed reads poll once per RTOS tick.** `softuart_read_bytes()` sleeps
  one tick (10 ms by default) between checks. The receive buffer must hold
  at least a tick's worth of data: about 10 bytes at 9600 baud, 115 bytes
  at 115200.

For high-speed or full-duplex links, use the hardware UART0 (with
`uart_enable_swap()` to move it to GPIO13/GPIO15) and keep the software
UART for slow peripherals.

## Project layout

```
.
├── components/softuart/       # the software UART library (ESP-IDF component)
│   ├── include/softuart.h     # public API
│   ├── softuart.c
│   ├── Kconfig                # menuconfig options
│   ├── CMakeLists.txt         # CMake build
│   └── component.mk           # legacy make build
├── main/                      # example application
│   ├── softuart_example_main.c
│   └── Kconfig.projbuild      # example pins, baud rate, inversion
├── test/host/                 # simulator-based unit tests (run on the build machine)
├── scripts/ci-build.sh        # build + stage firmware (used by CI)
├── sdkconfig.defaults         # project configuration defaults
├── CMakeLists.txt / Makefile  # project files for idf.py / make
└── .github/workflows/ci.yml   # GitHub Actions workflow
```

## Testing

`test/host/` contains unit tests that compile `components/softuart/softuart.c`
for the build machine against a small ESP8266 simulator. The simulator models virtual
time, GPIO levels and the GPIO ISR service. The tests cover:

- **Transmit.** Every byte value from 9600 to 115200 baud (and 230400),
  normal and inverted, decoded from the generated waveform. Every bit edge
  must sit within 5 % of a bit of its ideal position, with no glitches.
  Frames must start exactly 10 bits apart (plus the configured idle time).
- **Receive.** Every byte value from 9600 to 230400 baud, including
  back-to-back frames, delivered through the interrupt handler with
  simulated interrupt latency.
- **CPU clock.** Transmit and receive with the CPU at 80 MHz as well as
  160 MHz.
- **Timer wrap.** Frames in flight while the 32-bit microsecond timer
  crosses 2³¹ and wraps at 2³².
- **Lifecycle.** Opening, re-opening and closing ports, input validation,
  ring-buffer overflow, and GPIO ISR service ownership.
- **Receive API.** `softuart_read_byte()` with a `0x00` byte,
  `rx_count`, `rx_flush`, `rx_overflow`, and `softuart_read_bytes()` with
  buffered data, a zero timeout, an expiring timeout and data arriving
  while it sleeps. Also `softuart_printf()` with short and long output.

The suite runs twice: once with the default configuration, and once with
3 ports, a 16-byte buffer, 5 extra idle bits and the transmit critical
section enabled.

```sh
make -C test/host      # needs only a C11 compiler; runs with ASan and UBSan
```

These tests check the logic and timing arithmetic. They do not replace
testing on real hardware, where interrupt latency and Wi-Fi activity
matter.

## Continuous integration

Every push and pull request runs the host tests and builds the example
with both the legacy `make` and the CMake build systems. In CI, compiler
warnings in the library and the example are errors (`SOFTUART_WERROR=1`). The ESP8266_RTOS_SDK and the toolchain
versions are pinned. The firmware images are uploaded as build artifacts.
Pushing a `v*` tag also publishes a GitHub release with the images and a
`flash_args` file.

To reproduce a CI build locally:

```sh
scripts/ci-build.sh cmake   # or: scripts/ci-build.sh make
ls dist/                    # bootloader.bin, partition-table.bin, softuart-example.bin, ...
```

## Contributing

Issues and pull requests are welcome. Please:

1. Keep changes building with **both** `make` and `idf.py`
   (`scripts/ci-build.sh make && scripts/ci-build.sh cmake`), and keep the
   host tests passing (`make -C test/host`). Add a test for every bug fix.
2. Follow the existing code style (4-space indentation, braces on their own
   line in the library).
3. Describe how you tested the change, especially on real hardware, in the
   pull request.

## Credits and license

The library is a port of a long line of open-source software UARTs:

- 2015: [plieningerweb/esp8266-software-uart](https://github.com/plieningerweb/esp8266-software-uart), the original ESP8266 software UART.
- 2016 and 2017: Bernhard Guillon and Ruslan V. Uss reworked it for [esp-open-rtos](https://github.com/SuperHouse/esp-open-rtos/tree/master/extras/softuart).
- 2020: Ivan Belokobylskiy ([@devbis](https://github.com/devbis)) ported it to ESP8266_RTOS_SDK in [devbis/esp8266-rtos-softuart](https://github.com/devbis/esp8266-rtos-softuart).

This repository packages that port as a reusable component, fixes several
bugs, and adds tests, an example application and build tooling. It is released under the [MIT License](LICENSE). The source
files keep their original authors' copyright notices.
