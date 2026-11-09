# G106 hardware notes

Hardware map of the G106 as used by this firmware, reversed from the stock bootloader /
app V1.4b01 and verified on a real radio. Stock behaviour (EEPROM layout, UI, DSP) is in
[stock_spec.md](stock_spec.md).

MCU: STM32F405/407 (chip id 0x413), Cortex-M4F, 1 MB flash, 192 KB SRAM (incl. 64 KB CCM).
Flash layout: XG bootloader `0x08000000`–`0x0801FFFF` (128 KB), application from `0x08020000`.

## Bootloader → app contract (reversed from bootloader @ `0x0800164c`)

- Vector table at `0x08020000`, entered by a jump, not a reset: clocks, NVIC and GPIO are
  left in the bootloader's state.
- `(SP & 0xFFFF0000) == 0x20000000`, so the initial SP must be below `0x20010000`. The stack is
  therefore at the bottom of SRAM, as in the stock app.
- `(Reset & 0xFFF80000) == 0x08000000`, so `Reset_Handler` must be below `0x08080000`.
- **Bootloader bug: MSP is loaded with `*(SP & 0xFFFFF)`, not SP** (`0x080016a4`), i.e. garbage.
  `Reset_Handler` must set SP itself before touching the stack (stock `__main` does too).
- Bootloader leaves **DMA2 Stream5 running** (USART1 RX → `0x2000078c`) and `USART1 CR3.DMAR` set:
  the app disables clocked DMA streams at reset and clears USART1 CR2/CR3. Accessing an
  unclocked DMA controller bus-faults, so check RCC first.
- No CRC/signature check. No watchdog started.
- `.xgf` = plain AES-256-ECB of the raw bin, no header (`tools/xgf.py`).
- **IAP flashing sets RDP level 1 again** (option byte RDP = 0x55). With RDP1 a debugger attach
  blocks CPU flash reads and the running app locks up in a HardFault; check `mdw 0x40023C14`
  (byte 1 = 0xAA means RDP0) before using SWD. Back to RDP0 = `stm32f4x unlock 0` (mass erase!),
  then restore bootloader and app from your own backup.

## Clocks (same as stock)

HSI 16 MHz → PLL M=8 N=168 P=2 Q=4 → **168 MHz**. AHB 168 MHz, APB1 42 MHz, APB2 84 MHz,
FLASH 5 WS + I/D cache + prefetch, VOS scale 1.

## Pins

Confirmed:

| Pin | Dir | Function |
|---|---|---|
| PC7 | out | **power latch**, 1 = on. 0 or any MCU reset → power off (stock: `0x08029e96`) |
| PC8 | in, PU | power key, 0 = pressed |
| PC10 | out | LED (bootloader blinks it on fatal error) |
| PA9 / PA10 | AF7 | USART1 TX / RX |
| PA13 / PA14 | AF0 | SWD, never touch |

## Peripherals used by stock V1.4b01 (live register dump + static RE)

RCC enables: GPIOA–H, CRC, BKPSRAM, CCM, DMA1, DMA2, TIM2, SPI2, I2C1, I2C2, PWR, DAC, TIM1,
USART1, ADC1, SPI1, SYSCFG. NVIC: DMA1_Stream3, TIM1_UP_TIM10, TIM2, DMA2_Stream0.

| Block | Pins | Config | Device / role |
|---|---|---|---|
| I2S2 + I2S2ext | PB12 WS, PB13 CK, PB15 SD (RX), PB14 ext_SD (TX) | **MCU = slave** (codec is clock master), Philips, 32-bit data/frame | audio/IQ codec |
| DMA1 S3 ch0 | — | SPI2_RX → RAM, 16-bit, double buffer 2×256 B (32 stereo frames) | codec ADC in, IRQ = DSP tick |
| DMA1 S4 ch2 | — | RAM → I2S2ext_TX, double buffer 2×256 B | codec DAC out |
| I2C2 | PB10 SCL, PB11 SDA | 100 kHz | **TLV320AIC3204** @0x18 (page reg 0, PLL P/R/J/D, NDAC/MDAC/NADC/MADC), MCLK = 26 MHz |
| I2C1 | PB6 SCL, PB7 SDA | 100 kHz | **VersaClock 5/6** LO @0x6A; FM radio @0x60 in TEA5767 mode (IF 225 kHz) — also ACKs 0x10/0x11, so likely **RDA5807M**; **24Cxx EEPROM** @0x50 (16-bit addr); unknown @0x2E (1-byte write, digipot?) |
| SPI1 | PB3 SCK, PB5 MOSI, PB4 CS, PD2 CD | TX-only (BIDIOE), 8-bit, 10.5 MHz, CPOL0/CPHA0 | 128×64 mono LCD (UC1609-type); DMA2 S3 ch3 |
| USART1 | PA9 / PA10 | 115200, RX via DMA2 S2 ch4 circular | console / CAT |
| TIM1 CH4 | PA11 | 168 MHz/100/1000 = 1.68 kHz, 50 %, MOE gated | beeper |
| TIM2 | — | 1 kHz update IRQ | system tick |
| DAC1 | PA4 | static ~0.46 V (DHR=0x23A) | bias / reference, TBD |
| ADC1 | PA3 (analog) | idle, on-demand | TBD (SWR / power / battery) |
| GPIO scan | PC14, PC15, PH0 rows | strobed low in turn | key matrix |

### RF chain (from `radio_trx_task` 0x08029f90 / `rf_set_filters_for_freq` 0x0802b160)

- **LO: Renesas/IDT VersaClock 5/6** (5P49V59xx/69xx) on **I2C1 @0x6A** (8-bit 0xD4), 26 MHz xtal,
  VCO 2886 MHz. Output n divider: int 12 bit at reg `0x10*n+0x1D`, frac at `0x10*n+0x12`
  (`f_out = f_vco / (2*(int+frac/2^24))`). RX LO uses OUT1 = **4 × f** (Tayloe QSD).
- **Shift registers** (bit-banged, MSB first): data **PA1**, clock **PA0**, two latches:
  - latch **PA5**, one-hot: `0x08` <5.5 MHz, `0x04` <11.5, `0x02` <20, `0x01` ≥20 (BPF?)
  - latch **PA2**, one-hot `1<<n`: n=0 1.7–4.2 MHz, 1 5–7.6, 2 9.6–15, 3 16.5–22.5, 4 23.5–32,
    5 <65 MHz, 6 other / FM radio (LPF?)
  - power-off writes 0 to both.
- **DAC1 (PA4)** stepped with frequency: <2 MHz 285, <5 570, <7 856, <10 1141, <14 1427, <18 1712,
  <21 1997, <24 2283, <28 2568, <50 2854, else 3139 (of 4095). Likely varactor/preselector tuning.
- **TX on**: PB1=1, PA8=0, PC5=1, PA15=1, PC10 (LED)=0, codec switched to TX path.
  **RX**: PC5=0, PA8=1, PB1=0, PA15=0, PC10=1.
- PC9 follows bit 0 of the VFO flags (ATT/preamp?). PC11 toggled by a key, bit 0x100 of work params.
- Inputs: **PC2** debounced (PTT?), **PC4** (CW key?).

### EEPROM (24C64, 8 KB @0x50)

Holds the factory calibration — make a backup (`eedump` on the console) before experimenting.
Used areas: 0x0000, 0x0400–0x05FF, 0x0800, 0x0C00, 0x1000.

`0x400` bands (0xA8 B), `0x800` FM channels (0xD3 B, 87–108 MHz / 100 kHz, 0xAA = stored),
`0xC00` work params. Full layout in [stock_spec.md](stock_spec.md).

### Display (stock init @0x080259c8, DMA pump @0x08025abc; driver: `../src/display.c`)

- **128×64 monochrome LCD**, UltraChip **UC1609-family** command set
  (init: `E2, 2F, 81 38, EB, C7, 40, AF`). Page mode: `B0+page, 10, 00` then 128 data bytes.
- SPI1 TX-only mode 0, 10.5 MHz: PB3 SCK, PB5 MOSI. **PB4 = CS** (active low),
  **PD2 = CD/A0** (0 cmd, 1 data), **PA12 = RST** (held high), **PC11 = backlight** (likely).
- Stock framebuffer at `0x20002998` (8 × 128 B, bit 0 = top row), refreshed one page per tick.
  With mapping `0xC7` column 0 is the **right** edge of the glass, so the framebuffer is X-mirrored.

### Front panel (driver: `../src/input.c`, verified on hardware)

- Key matrix 3×3: rows PH0 / PC15 / PC14 (driven low in turn), columns PC1 / PC0 / PH1 (active low).
  Code = `col_bit | 0x100 << row` → 9 keys `0x101 … 0x404` (same codes as stock):
  MODE `0x401`, BAND− `0x402`, BAND+ `0x404`, buttons 1–4 under the LCD `0x101 0x102 0x104 0x201`,
  tune encoder push `0x202`, volume encoder push `0x204`.
- Encoders: tune A=PC13 B=PB9; aux (volume in stock) A=PB8 B=PC12.
- **PTT = PC2** (active low). PC3 and PB2 read low at idle (PB2 = BOOT1 strap).
  PA6 / PA7 pulse low at power-up only (CW key jack?), PC4 never changed. Key not tested (none).
- Beeper TIM1 CH4 / PA11 works.

Not found yet: digipot @0x2E role, CW key inputs.

Live dump of stock V1.4b01 while running (mode / output level). Roles still to be reversed:

```
A 0:OUT=0 1:OUT=1 2:OUT=0 3:an 4:an 5:OUT=0 6:in 7:in 8:OUT=1 9:AF 10:AF 11:AF 12:OUT=1 13:AF 14:AF 15:OUT=0
B 0:OUT=1 1:OUT=0 2:in 3:AF 4:OUT=1 5:AF 6:AF 7:AF 8:in 9:in 10:AF 11:AF 12:AF 13:AF 14:AF 15:AF
C 0:in 1:in 2:in 3:in 4:in 5:OUT=0 6:OUT=1 7:OUT=1 8:in 9:OUT=1 10:OUT=1 11:OUT=1 12:in 13:in 14:OUT=0 15:OUT=1
H 0:OUT=1 1:in ...
```

Bootloader-set outputs: PA12=1, PC6=1, PC7=1, PC10=1, PA15=0, PB1=0, PC9=0, PC15=0.
The stock app keeps its pins as a linked list of objects in RAM `0x200007a4`..`0x20000cfc`
(`{prev, next, GPIOx, pinmask, cfg, ...}`); code that references an object's address shows
what the pin does.
