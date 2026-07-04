/* example.c — minimal demo of the rp2350-usb-cdc driver.
 *
 * Brings up USB, then prints a heartbeat once a second. Open the serial port that appears
 * (/dev/cu.usbmodem* on macOS, /dev/ttyACM* on Linux, a COM port on Windows) at any baud rate.
 *
 * It also demonstrates input: type e.g.  read 0x40010000 16  <enter>  and the default RX handler
 * (in usb_cdc.c) dumps 16 bytes starting at that address back over the serial link. Try
 * `read 0x40058000 16` to read the PLL_USB registers, etc. */
#include "usb_cdc.h"

int main(void)
{
    usb_cdc_init();                       /* clocks + enumerate as a USB serial port */

    uint32_t beat = 0, spin = 0;
    for(;;){
        usb_cdc_task();                   /* MUST be called often — the driver is polled */

        /* ~1 Hz heartbeat. clk_sys is 48 MHz here, so this loop is a crude delay; use a timer for
         * anything real. This just keeps the example self-contained. */
        if(++spin >= 3000000u){
            spin = 0;
            usb_cdc_puts("rp2350-usb-cdc beat ");
            usb_cdc_hex(beat++);
            usb_cdc_puts("\r\n");
        }
    }
}
