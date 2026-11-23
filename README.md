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
- 8 data bits, no parity, 1 stop bit (8N1).
- Optional inverted line logic, for links that idle low.
- Interrupt-driven receive into a per-port ring buffer (64 bytes by default).
- Blocking transmit of single bytes, C strings or byte buffers.
- Builds with both the legacy GNU Make and the CMake (`idf.py`) build systems.

## How it works

```
 idle  start   D0   D1   D2   D3   D4   D5   D6   D7   stop  idle
 ────┐      ┌────┐    ┌──────────────┐    ┌────┐    ┌─────────────
     └──────┘    └────┘              └────┘    └────┘
      1 bit  ◄─────── 8 data bits, LSB first ───────► 1 bit
```

**Receive.** The RX pin has an edge interrupt that fires on the falling
edge of the start bit (rising edge when inverted). The handler waits half a
bit time, then samples the 8 data bits in the middle of each bit period.
It pushes the byte into the port's ring buffer, waits for the stop bit and
re-arms the interrupt. `softuart_available()` and `softuart_read()` drain
the buffer from task context.

**Transmit.** `softuart_put()` drives the TX pin from the calling task. Each
bit edge is timed against the SDK's microsecond system timer, measured from
the start of the frame, so timing errors do not accumulate across a byte.

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

The example opens software UART 0 on RX = GPIO4, TX = GPIO5 at 74880 baud.
It repeatedly sends `Hello from Soft UART!` and prints received bytes
to the console. Connect a USB-serial adapter as shown under
[Wiring](#wiring) and open it at 74880 8N1 (for example
`picocom -b 74880 /dev/ttyUSB1`).

## Using the library

Copy `main/softuart.c` and `main/softuart.h` into a component of your
project, then:

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
        while (softuart_available(SENSOR_UART)) {
            putchar(softuart_read(SENSOR_UART));
        }
        vTaskDelay(pdMS_TO_TICKS(10));   // yield so the idle task and watchdog run
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
| `bool softuart_available(uint8_t uart_no)` | `true` if at least one received byte is waiting in the buffer. |
| `uint8_t softuart_read(uint8_t uart_no)` | Pop the next received byte. Returns `0` if the buffer is empty, so call `softuart_available()` first. |

## Configuration

Both limits are compile-time macros in `softuart.h` and can be overridden
with compiler definitions:

| Macro | Default | Meaning |
|---|---|---|
| `SOFTUART_MAX_UARTS` | `2` | Number of software UART ports |
| `SOFTUART_MAX_RX_BUFF` | `64` | Receive ring-buffer size per port, in bytes. Must be a power of two from 2 to 256 (checked at compile time). One slot stays free, so a port holds up to `SOFTUART_MAX_RX_BUFF - 1` bytes. When the buffer is full, newer bytes are dropped. |

```cmake
# CMake: in the component's CMakeLists.txt, after idf_component_register()
target_compile_definitions(${COMPONENT_LIB} PRIVATE SOFTUART_MAX_RX_BUFF=128)
```

```make
# Legacy make: in the component's component.mk
CFLAGS += -DSOFTUART_MAX_RX_BUFF=128
```

`softuart.c` defines `SOFTUART_DEBUG`, which prints configuration errors
(such as an invalid port number) with `printf`.

## Limitations

A software UART uses the CPU to time every bit, and that has costs:

- **The CPU is busy for every byte.** Receiving a byte holds the CPU in the
  GPIO interrupt for about 9.5 bit times (≈ 1 ms at 9600 baud).
  Transmitting busy-waits in the calling task for the length of the frame
  plus some idle time.
- **In practice the link is half-duplex.** A byte arriving while
  `softuart_put()` is running interrupts the transmitter and distorts the
  outgoing bits. Two ports also cannot receive at the same moment.
- **Interrupt latency limits the speed.** Wi-Fi and other interrupts can
  delay the start-bit interrupt or stretch a transmitted bit. 9600 baud is
  the proven use case (a GPS module, by the original authors).
  Reliability drops as the baud rate rises.
- **Bit timing has 1 µs resolution.** The bit time is rounded to a whole
  microsecond, which adds a rate error at high baud rates (for example
  13 µs instead of 13.35 µs at 74880 baud).
- **Only 8N1.** There is no parity, no 2-stop-bit mode and no flow control.
- **A received NUL byte looks like "no data".** `softuart_read()` returns
  `0` for both, so check `softuart_available()` first.

For high-speed or full-duplex links, use the hardware UART0 (with
`uart_enable_swap()` to move it to GPIO13/GPIO15) and keep the software
UART for slow peripherals.

## Project layout

```
.
├── main/
│   ├── softuart.c / .h        # the software UART library
│   ├── hello_world_main.c     # example application
│   ├── CMakeLists.txt         # component registration (CMake build)
│   └── component.mk           # component makefile (legacy make build)
├── test/host/                 # simulator-based unit tests (run on the build machine)
├── scripts/ci-build.sh        # build + stage firmware (used by CI)
├── sdkconfig.defaults         # project configuration defaults
├── CMakeLists.txt / Makefile  # project files for idf.py / make
└── .github/workflows/ci.yml   # GitHub Actions workflow
```

## Testing

`test/host/` contains unit tests that compile `softuart.c` for the build
machine against a small ESP8266 simulator. The simulator models virtual
time, GPIO levels and the GPIO ISR service. The tests cover:

- **Transmit.** Every byte value at 9600 and 38400 baud, normal and
  inverted, decoded from the generated waveform. Every bit edge must sit
  within 5 % of a bit of its ideal position, with no glitches.
- **Receive.** Every byte value, including back-to-back frames, delivered
  through the interrupt handler with simulated interrupt latency.
- **Timer wrap.** Frames in flight while the 32-bit microsecond timer
  crosses 2³¹ and wraps at 2³².
- **Lifecycle.** Opening, re-opening and closing ports, input validation,
  ring-buffer overflow, and GPIO ISR service ownership.

```sh
make -C test/host      # needs only a C11 compiler; runs with ASan and UBSan
```

These tests check the logic and timing arithmetic. They do not replace
testing on real hardware, where interrupt latency and Wi-Fi activity
matter.

## Continuous integration

Every push and pull request runs the host tests and builds the example
with both the legacy `make` and the CMake build systems. The ESP8266_RTOS_SDK and the toolchain
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

This repository packages that port with an example application and build
tooling. It is released under the [MIT License](LICENSE). The source
files keep their original authors' copyright notices.
