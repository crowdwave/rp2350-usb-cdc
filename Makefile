# rp2350-usb-cdc — bare-metal RP2350 USB CDC serial. Build with arm-none-eabi-gcc, flash with picotool.
#
#   make            # build example.uf2 + example.elf
#   make flash      # flash to a board in BOOTSEL mode (hold BOOTSEL, plug in)
#   make flash-f    # flash a running board via the picotool reset interface (no button)
#   make clean

CROSS    = arm-none-eabi-
CC       = $(CROSS)gcc
SIZE     = $(CROSS)size
PICOTOOL = picotool

MCU      = -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
CFLAGS   = $(MCU) -O2 -ffreestanding -nostdlib -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS  = -T rp2350.ld -Wl,--gc-sections -Wl,-Map=example.map

SRCS     = startup_rp2350.c usb_cdc.c example.c

all: example.uf2

example.elf: $(SRCS) usb_cdc.h rp2350.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lgcc
	$(SIZE) $@

example.uf2: example.elf
	$(PICOTOOL) uf2 convert $< $@

# Board must be in BOOTSEL (hold BOOTSEL while plugging in USB, or double-tap RESET on some boards).
flash: example.uf2
	$(PICOTOOL) load -x $<

# Reboot a running board to BOOTSEL via the reset interface, then flash — no button press.
flash-f: example.uf2
	$(PICOTOOL) load -f -x $<

clean:
	rm -f *.elf *.uf2 *.map

.PHONY: all flash flash-f clean
