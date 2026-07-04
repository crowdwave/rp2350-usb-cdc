/* usb_cdc.h — minimal bare-metal USB CDC-ACM serial for the RP2350 (no SDK / TinyUSB / interrupts).
 * See README.md. MIT licensed. */
#ifndef USB_CDC_H
#define USB_CDC_H
#include <stdint.h>

/* Set up the clocks (clk_usb + clk_sys, both 48 MHz) and enumerate as a USB CDC serial device.
 * Call once at startup, before usb_cdc_task(). Blocks only briefly (bounded waits, can't hang). */
void usb_cdc_init(void);

/* Service the USB controller. Call frequently from your main loop — the driver is fully polled,
 * it uses no interrupts, so nothing happens on USB unless you call this. */
void usb_cdc_task(void);

/* Send to the host. Bytes are buffered (2 KB ring) and flushed by usb_cdc_task(). Non-blocking;
 * bytes are dropped if the ring is full and the host isn't reading. */
void usb_cdc_putc(char c);
void usb_cdc_puts(const char *s);
void usb_cdc_write(const void *data, uint32_t len);
void usb_cdc_hex(uint32_t v);                 /* prints "0xXXXXXXXX" */

/* Called once per byte received from the host, from inside usb_cdc_task(). It is a weak symbol:
 * define your own usb_cdc_on_rx() to handle input. The built-in default implements a small
 * "read <hexaddr> <declen>\n" command that dumps memory/registers back over the serial link. */
void usb_cdc_on_rx(char c);

/* Reboot into the ROM USB bootloader (BOOTSEL / mass-storage flashing mode). This is also invoked
 * automatically when a host tool (e.g. `picotool ... -f`) sends the reset request to our vendor interface. */
void usb_cdc_reboot_bootsel(void);

#endif /* USB_CDC_H */
