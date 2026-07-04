# rp2350-usb-cdc

**A tiny, bare-metal USB serial (CDC-ACM) driver for the RP2350 — no pico-SDK, no TinyUSB, no interrupts, no RTOS.**

Drop three files into a bare-metal RP2350 project and your firmware shows up on the host as a normal USB serial port (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux, a COM port on Windows). You get `printf`-style output over USB and a byte stream in — perfect for `printf`-debugging, telemetry, and command/console interfaces on a chip that otherwise gives you no easy way to see what your code is doing.

It also implements the **picotool reset interface**, so you can reflash a *running* board with `picotool ... -f` (no BOOTSEL button), and a one-line call to jump to the ROM bootloader yourself.

The whole thing is ~250 lines of C driving the USB controller with raw register writes. It's **polled** — you call `usb_cdc_task()` from your main loop — so it needs no interrupt handlers and stays out of the way of hard-real-time code on the other core.

> This exists because bringing up the RP2350 USB device from scratch, without the SDK, hides a nasty non-obvious gotcha (see [Why this was hard](#why-this-was-hard)). This driver has that fix baked in, so you don't have to rediscover it.

Tested working on a **Raspberry Pi Pico 2 / Pico 2 W** and a **Waveshare RP2350-PiZero**, flashed with `picotool`, read with a plain serial terminal.

---

## What you get

- **USB CDC-ACM serial device** — enumerates as a standard serial port at any baud (CDC ignores the baud rate).
- **TX**: `usb_cdc_puts()`, `usb_cdc_putc()`, `usb_cdc_write()`, `usb_cdc_hex()` — buffered, non-blocking.
- **RX**: an overridable `usb_cdc_on_rx(char)` callback (weak symbol). The built-in default is a small
  `read <hexaddr> <declen>` command that dumps memory/registers back over serial — instant hardware peeking.
- **`picotool -f` support** — the picotool *reset* vendor interface, so a running board can be rebooted to
  BOOTSEL and reflashed without physically pressing anything.
- **`usb_cdc_reboot_bootsel()`** — jump to the ROM USB bootloader from your own code.
- **No dependencies** — just `arm-none-eabi-gcc`. No SDK, no CMake, no submodules. ~2.7 KB of flash.
- **No interrupts** — everything runs from `usb_cdc_task()`, so it composes cleanly with bare-metal
  real-time code (e.g. run it on core 0 while core 1 does something timing-critical).

## Files

| File | What it is |
|---|---|
| `usb_cdc.c` / `usb_cdc.h` | the driver — **this is the reusable part** |
| `startup_rp2350.c` | minimal reset vector, RP2350 `IMAGE_DEF` boot block, `.data`/`.bss` init, calls `main()` |
| `rp2350.ld` | linker script (flash @ `0x10000000`, SRAM @ `0x20000000`) |
| `example.c` | minimal demo: heartbeat out + the memory-read command in |
| `Makefile` | build with `arm-none-eabi-gcc`, flash with `picotool` |

`startup_rp2350.c` and `rp2350.ld` are a bare-minimum bring-up so the example is self-contained; if you already have your own startup/linker for the RP2350, you only need `usb_cdc.c` + `usb_cdc.h`.

## Requirements

- [`arm-none-eabi-gcc`](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (tested with 14.2)
- [`picotool`](https://github.com/raspberrypi/picotool) (v2.x) for flashing / UF2 conversion
- Any RP2350 board (Pico 2, Pico 2 W, and clones)

## Quick start

```sh
make                 # -> example.uf2

# Flash: hold BOOTSEL while plugging in USB (board mounts as an RP2350 drive), then:
make flash           # picotool load -x example.uf2
```

Then open the serial port that appears:

```sh
# macOS
screen /dev/cu.usbmodem* 115200      # (baud is ignored; any value works)
# Linux
screen /dev/ttyACM0 115200
```

You'll see:

```
rp2350-usb-cdc beat 0x00000000
rp2350-usb-cdc beat 0x00000001
...
```

Type a command and press enter to read memory/registers live:

```
read 0x40058000 16
0x40058000: 0x00000001 0x00000000 0x00000078 0x00050000
```

Once it's running, you can reflash **without the button** (the reset interface handles it):

```sh
make flash-f         # picotool load -f -x example.uf2
```

## Using it in your own project

```c
#include "usb_cdc.h"

int main(void) {
    usb_cdc_init();                    // clocks + enumerate. call once.
    for (;;) {
        usb_cdc_task();                // call OFTEN — the driver is polled, no IRQs
        // ... your code ...
        usb_cdc_puts("hello\r\n");     // buffered, non-blocking
    }
}
```

Handle input by defining your own `usb_cdc_on_rx` (overrides the built-in memory-dump default):

```c
void usb_cdc_on_rx(char c) {
    usb_cdc_putc(c);                   // e.g. a simple echo
}
```

**Important:** `usb_cdc_init()` sets `clk_sys` **and** `clk_usb` to 48 MHz (see below). If your application
needs a different or faster system clock, set it up **after** `usb_cdc_init()` — but keep `clk_sys >= 48 MHz`,
or the USB controller stops working. Running from XIP flash at higher clocks may need the QMI timing adjusted.

## API

| Function | |
|---|---|
| `void usb_cdc_init(void)` | set up clocks + enumerate as a serial port. Call once. |
| `void usb_cdc_task(void)` | service USB. Call frequently from your main loop. |
| `void usb_cdc_putc(char)` / `usb_cdc_puts(const char*)` / `usb_cdc_write(const void*, uint32_t)` | send (buffered). |
| `void usb_cdc_hex(uint32_t)` | send `"0xXXXXXXXX"`. |
| `void usb_cdc_on_rx(char)` | **weak** — override to handle received bytes. |
| `void usb_cdc_reboot_bootsel(void)` | jump to the ROM USB bootloader. |

## Why this was hard

Every register can be correct and the device still won't enumerate, because of one thing the SDK/TinyUSB
hide from you:

**`clk_sys` must be ≥ `clk_usb` (48 MHz).** The RP2350's USB SIE reads its packet buffers and control
registers on `clk_sys`. The bootrom hands control to your firmware with `clk_sys` running on the ~8 MHz
ring oscillator — *slower than the 48 MHz USB clock* — and in that state the controller never even asserts
the D+ pull-up, so the host sees **nothing** (no device, no error, no SETUP packets). pico-SDK and TinyUSB
never hit this because they always run `clk_sys` fast (~125 MHz). This driver raises `clk_sys` to 48 MHz in
`usb_cdc_init()`, which is the fix.

Other RP2350-specific requirements that are easy to get wrong (all handled here):

- **Buffer-control `AVAILABLE` is a two-step write** (datasheet §12.7.3.7.1): write length/PID/FULL, wait
  ≥12 cycles, *then* set `AVAILABLE`. One write = malformed transfer.
- **`USB_INTS` is masked by `USB_INTE`** — even when polling (not using the IRQ) you must set `INTE`, or the
  status bits you poll read as zero forever.
- **EP0 control-read status stage**: after sending descriptor data you must arm EP0 OUT for the zero-length
  status packet, or descriptor reads never complete and the device shows as "unknown."
- **Don't set `SIE_CTRL.RPU_OPT`** — it selects a non-standard pull-up option and breaks enumeration on
  ordinary boards.
- **`PHY_ISO`** (the USB PHY isolation bit) defaults to *isolated*; writing `MAIN_CTRL = CONTROLLER_EN`
  clears it as a side effect.
- **DPRAM hates widened accesses** — copy to/from the USB DPRAM byte-by-byte (`volatile`), or aligned
  32-bit; a compiler-widened byte copy can hard-fault.

## Notes & limitations

- The USB VID/PID is `0x2e8a:0x000a` (Raspberry Pi's). Keeping it is what lets `picotool -f` recognise and
  reset the device. For a shipping product you should use your own VID/PID (and then `picotool -f` won't apply).
- Full-speed (12 Mbit/s) CDC. Single interface, single serial port. TX is a 2 KB ring; bytes are dropped if
  the host isn't draining and the ring fills (typical for "nobody has the port open" — that's fine).
- The driver assumes it owns the USB controller and (in `usb_cdc_init`) the XOSC, PLL_USB, `clk_usb` and
  `clk_sys`. It does not touch PLL_SYS.
- This is a debugging/telemetry tool, not a certified USB stack. It implements just enough of CDC-ACM to be
  a serial port.

## Credits

The enumeration structure follows the pico-examples `dev_lowlevel` minimal USB device; the 48 MHz clock
recipe and several fixes were cross-checked against a bare-metal RP2350 reference and TinyUSB's
`dcd_rp2040.c`. Register values are from the pico-sdk RP2350 headers and the RP2350 datasheet.

## License

MIT — see [LICENSE](LICENSE).
