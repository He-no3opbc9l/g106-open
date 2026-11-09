TARGET   := g106_open
BUILD    := build
PORT     ?= /dev/ttyUSB0

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

# Official stock firmware (download it from the manufacturer and put it into stock/). Needed at
# build time: the original UI fonts/icons are extracted from it. Not part of this repository.
STOCK_XGF := stock/G106Fw-V1.4b01.xgf
STOCK     := stock/G106Fw-V1.4b01.bin
STOCK_TX  := stock/G106Fw-V1.4b01.txall.bin
SRCS     := $(wildcard src/*.c)
OBJS     := $(SRCS:src/%.c=$(BUILD)/%.o) $(BUILD)/stock_gfx.o

ARCH     := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS   := $(ARCH) -std=c11 -O2 -ffp-contract=fast -g3 -Wall -Wextra -ffunction-sections -fdata-sections \
            -ffreestanding -MMD -MP
LDFLAGS  := $(ARCH) -T ld/g106_app.ld -nostartfiles --specs=nano.specs --specs=nosys.specs \
            -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map
LDLIBS   := -lm

.PHONY: all clean flash-swd flash-iap restore-stock-swd stock FORCE

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).xgf

# __DATE__/__TIME__ (banner, splash, VER) must match the image: always recompile their users.
$(BUILD)/main.o $(BUILD)/radio.o: FORCE
FORCE:

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(STOCK): $(STOCK_XGF) tools/xgf.py
	python3 -I tools/xgf.py dec $< $@

# Stock app with the amateur-band TX limit removed (what restore-stock-swd writes).
$(STOCK_TX): $(STOCK) tools/patch_txall.py
	python3 -I tools/patch_txall.py $< $@

stock: $(STOCK) $(STOCK_TX)

$(STOCK_XGF):
	@echo "missing $@: download the official G106 V1.4b01 firmware and put the .xgf there"; exit 1

# Original UI fonts/icons, extracted from the stock image at build time (not in src/).
$(BUILD)/stock_gfx.c: $(STOCK) tools/extract_stock_gfx.py | $(BUILD)
	python3 -I tools/extract_stock_gfx.py $(STOCK) $@

$(BUILD)/stock_gfx.o: $(BUILD)/stock_gfx.c src/gfx_data.h
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) ld/g106_app.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).xgf: $(BUILD)/$(TARGET).bin
	python3 -I tools/xgf.py enc $< $@

$(BUILD):
	mkdir -p $@

# Halts the CPU (no reset!), writes the app from 0x08020000, then powers the radio off via PC7.
flash-swd: $(BUILD)/$(TARGET).bin
	tools/flash_swd.sh $<

# Stock-bootloader IAP over USART1 (builds first). Radio already in the IAP menu: flashes at once;
# otherwise power it on holding the volume button. Note: IAP sets RDP1 again (SWD then unusable).
flash-iap: $(BUILD)/$(TARGET).xgf
	@if fuser $(PORT) >/dev/null 2>&1; then echo "$(PORT) is busy (close the terminal/script using it)"; exit 1; fi
	python3 -I tools/flash_iap.py $(PORT) $<

restore-stock-swd: $(STOCK_TX)
	tools/flash_swd.sh $<

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
