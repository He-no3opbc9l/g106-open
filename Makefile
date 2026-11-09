TARGET   := g106_open
BUILD    := build
PORT     ?= /dev/ttyUSB0

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

# Official stock firmware, needed at build time: the original UI fonts/icons are extracted from
# it. Not part of this repository: downloaded from the official update archive on first build
# (or put G106Fw-V1.4b01.xgf into stock/ by hand), checked by sha256.
STOCK_URL := https://radioddity.s3.amazonaws.com/2025-07-03_G106_Firmware_update_V1.4.zip
STOCK_SHA := 95bd0b0adea0027cd75855aa280ad97ee8abd17620786a5e6d2c23f8dc01d113
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
	mkdir -p $(@D)
	curl -sSLf -o $(@D)/stock.zip $(STOCK_URL)
	unzip -p $(@D)/stock.zip $(@F) > $@.tmp
	@echo "$(STOCK_SHA)  $@.tmp" | sha256sum -c --quiet || { rm -f $@.tmp; echo "unexpected $(@F)"; exit 1; }
	mv $@.tmp $@
	rm -f $(@D)/stock.zip

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
