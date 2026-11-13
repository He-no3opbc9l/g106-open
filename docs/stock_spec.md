# Stock G106 V1.4b01 behaviour

Goal: re-implement the stock firmware's behaviour clean-room, then evolve it. Addresses refer to
the stock app image (base `0x08020000`; Ghidra project from `tools/ghidra/run.sh`).
Hardware/pin details are in [hardware.md](hardware.md).

## Architecture

- `main` (0x080305a4): clocks, GPIO from the pin-object list (each pin has an initial level),
  DAC, display, TEA5767, I2S, codec init + RX routing, TIM2 1 ms tick, then `boot` (0x0803458c).
  Codec trims at the end of init: MICPGA 0x1E (+15 dB), ADC digital volume +10 (5 dB),
  LOL/LOR gain 0x10 (+16 dB).
- Main loop: `spectrum_task` (0x0802d404) + `ui_task` (0x080341cc), forever.
- TIM2 1 ms ISR: display DMA pump (one LCD page per tick), key matrix scan, encoders, beeper sequencer.
- DMA1 Stream3 ISR (`DMA1_Stream3_IRQHandler_audio` 0x0802dbxx): all DSP, 32-frame blocks at 48 kHz.
- GUI: Segger emWin (fonts/bitmaps extracted at build time, see `tools/extract_stock_gfx.py`).

## Modes (index used everywhere, e.g. VFO.mode)

| idx | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| name | LSB | L-D | USB | U-D | CW | CWR | NFM | AM |

(`mode_name` 0x0802fbc4). L-D/U-D = data SSB.

## EEPROM (24C64) layout — verified against the radio's EEPROM (all CRCs match)

CRC = STM32 hardware CRC unit: poly 0x04C11DB7, init 0xFFFFFFFF, 32-bit words, no reflection.

| addr | size | RAM | content |
|---|---|---|---|
| 0x0000 | 400 | 0x200020bc | 50 memory channels × 8 B `{u16 ?, u16 flags (0xAAxx = valid), u32 freq}` |
| 0x0400 | 0xA8 | 0x2000224c | 21 band slots × 8 B `{u32 info (mode etc.), u32 last freq}`; defaults at flash 0x08038a94 |
| 0x0500 | 0x12 | 0x200003f4 | call sign: `0xAA, len, char[16]` (shown on boot instead of the brand name) |
| 0x0540 | 0x44 | 0x20001fc4 | settings, CRC over 16 words at +0x40 (see below) |
| 0x0800 | 0xD3 | 0x200022f4 | FM channels: 1 byte per 100 kHz 87.0–108.0 MHz, 0xAA = stored |
| 0x0C00 | 0x30 | 0x2000205c | work params, CRC over 11 words at +0x2C (see below) |
| 0x1000 | 8 | 0x2000037c | param2, CRC over 1 word at +4 |

### Work params (0x0C00)

| off | type | meaning | default |
|---|---|---|---|
| 0x00 | u16, u16 mode, u32 freq | VFO A | mode 2 (USB), 14.270.000 |
| 0x08 | u16, u16 mode, u32 freq | VFO B | mode 0 (LSB), 7.050.000 |
| 0x10 | u16 flags | bit0 memory mode, bit1 lock, bit4 beep, bit5 FM radio, bit7 PB0 out, bit8 PC11 backlight | 0x100 |
| 0x12 | i16 | tuning step index 0..4 | 0 |
| 0x14 | u8 | band index | |
| 0x15 | u8 | memory channel (0..49) | 0 |
| 0x16 | u8 | current VFO (0 = A, 1 = B) | 0 |
| 0x18 | i16 | volume 0..46 (digipot via 0x0802b3d4) | 24 |
| 0x1A | i16 | 0..2 (DSP option, -> 0x10000003) | 2 |
| 0x1C | u32 | FM radio frequency | 95.800.000 |
| 0x24..0x28 | | 0x20002080 = 3, 0x20002082 = 2, 0x20002084 = 4 | |
| 0x2C | u32 | CRC | |

### Settings (0x0540)

| RAM | default | notes |
|---|---|---|
| 0x20001fc4 i16 | 800 | CW tone / scope centre (Hz) |
| 0x20001fc6 i16 | 100 | 0..1000 |
| 0x20001fc8 i16 | 2 | 0..4 |
| 0x20001fca i16 | 0 | 0/1/2 -> scope span 50/250/500 Hz (CW filter?) |
| 0x20001fcc u8 | 1 | |
| 0x20001fcd u8 | 0 | split mode (2 hides second VFO line) |
| 0x20001fce u8 | 20 | key speed? |
| 0x20001fcf u8, 0x20001fd0 u8 | 0, 1 | |
| 0x20001fd4, 0x20001fd8 | 0 | TX IQ calibration (I/Q DC offsets -> 0x10000018/1c) |
| 0x20001fdc | 10 × {i16, i16} | per-band TX IQ cal |
| 0x20002004 | | CRC |

### Param2 (0x1000)

0x2000037c u8 = 20 (max 30; TX power?), 0x2000037d flags (bit0 ...), CRC at +4.

## Boot (0x0803458c)

1. MODE held at power-on (key 0x401): "FACTORY RESET?" YES (F1/F2) / NO (F3/F4) -> writes defaults.
2. Read work params + memories; CRC mismatch -> "BAD CRC" screen, defaults, "INIT OK!".
3. TEA5767 init, VersaClock init, volume, backlight (PC11 = flags bit8), PB0 = flags bit7.
4. Boot screen: backlight, rising beep 261.6/329.6/392 Hz x 100 ms (0x080389b0, if beep on), 48x48
   logo 0x080392c0 @(0x27,0), call sign (or the brand name) in font8 centred in (0,49)-(127,63), 3 s.
5. Bands validated against band edges (`band_of` 0x08031b98), settings/param2 CRC-checked.
6. Load VFO (or memory channel), draw main screen (0x08034374), unmute (HPL 0x1C).

## Main screen (0x08034374)

- icon @(2,1) always; mode text @(26,1); icons @(14,1) VFO flag bit7, @(50,1) "P" VFO flag bit0,
  @(63,1) lock, @(86,1) "SPL", @(109,1) "QSK" (status bits 0x2000209c).
- VFO label @(3,19) / @(3,31), main frequency @(13,10) big font with step digit inverted,
  other VFO @(16,31) small font (hidden in split mode 2).
- S-meter @y42 (0x0802f848): S0 = -121 dBm, 6 dB/S unit, bar x 25..126, peak hold 50 ticks.
- Spectrum (0x0802d404): 2048-pt FFT over 32 audio blocks, window table 0x080384ec,
  128 columns of 8 bins, log scale, auto reference level.

## UI loop (0x080341cc)

Encoder 0 tunes (step table 0x0803113c, limits 550 kHz..30 MHz) or selects memory channel in
memory mode; key combo 0x406 toggles lock; 0x202 (tune push) -> 0x08031a98; other keys ->
0x08031d88. Periodic redraw: 0x08033400, 0x08031688. SYSTEM CAL item table 0x08038c2c (see SYSTEM CAL below).

## Keys (0x08031d88) — short press acts on release, long press after ~1 s

| key | short | long |
|---|---|---|
| MODE 0x401 | next enabled mode (table 0x08038a54: CWR, NFM disabled) | toggle preamp (VFO flags bit0 -> PC9) |
| BAND- 0x402 / BAND+ 0x404 | band slot -/+ (ham slots only unless FULL BAND), or memory ch in memory mode | step digit +1 / -1 (wraps 0..7) |
| BAND- + BAND+ 0x406 | lock | |
| tune push 0x202 | open / close soft-key panel | |
| vol push 0x204 | toggle flags bit7 -> PB0 | |
| F1..F4 0x101 0x102 0x104 0x201 | soft key of current page; first press with the panel closed only opens it | second function (only QSK) |

Band change saves the current VFO into its slot and loads the target slot (validated, else default).

## Soft keys (table 0x08038c44: page -> 4 × {label, short fn, long fn})

| page | F1 | F2 | F3 | F4 |
|---|---|---|---|---|
| 1 | V/M 0x08032d61 | A/B 0x08032ce5 | MW 0x08032299 | MC 0x08032025 |
| 2 | CWF 0x08032949 | CWT 0x080329c1 | CWR 0x080329b5 | QSK 0x08032c0d / long 0x080329a9 |
| 3 | KS 0x08032ae9 | KM 0x08032add | IMB 0x08032ad1 | CSN 0x0803186d |
| 4 | SPL 0x08032c95 | DIS 0x08032ac5 | BP 0x080328e1 | VER 0x0803198d |
| 5 | WFM 0x08033991 | BSM 0x08032909 | MG 0x08032b1d | TXP 0x08032cbd |

Panel (0x080318b4, 0x0802f67c): clears y 29..63, redraws the second VFO line, "[page/5]" @(6,45),
double rule y 48-49, four 32 px cells with centred labels @y 54. While open the tune encoder
selects the page; closes on timeout (~200000 loop passes) or tune push. Pop-up messages
("BEEP ON/OFF", "HAM/FULL BAND") replace the second-VFO line @(3,31) for a while.

Display mode DIS (settings +9): 0 = S-meter + 15-row scope, 1 = 22-row scope,
2 = 34-row scope and no second-VFO line. Scope marker at x 63 (CW: +tone·128/48000 px, CWR: −):
shown for 50 scope draws (counter 0x20000338) after the displayed frequency changes; 5/3/1 px
triangle in the top 3 rows, gap, bar of height-4 to the bottom; columns ±2 clamped to height-4.

Top-left icon is RX (0x08039350) / TX (0x08039380), not a VFO letter.

## Memories (V/M 0x08032d60, MW 0x08032298, MC 0x08032024, find 0x08032634)

Record = VFO record `{u16 flags (0xAA00 = valid, bit0 preamp), u16 mode, u32 freq}` at EEPROM `ch*8`, 50 channels.

- V/M: toggles flags bit0. Entering searches the first valid channel from the current one
  (forward, wrapping); none -> stays in VFO mode. In memory mode the tune encoder and BAND±
  step through valid channels; "MEMO" @(85,31) + 2-digit channel @(115,31).
- MW: modal. Labels QUIT · · SAVE (table 0x08038d04). "MW" blinks @(85,31), channel @(115,31),
  inverted "E" icon (0x08039320) @(99,33) if the channel is empty. Tune encoder: any channel
  0..49. F1 quit, F4 write current VFO (flags low byte kept, marker 0xAA). Error -> message box
  "ERROR" / "CAN'T WRITE MEMORY!".
- MC: needs a valid channel (else "INFO" / "NO VALID MOMOEY!" [sic]). Shows the channel's
  frequency (no step highlight), mode and P icon. Labels QUIT · · CLR. Tune encoder: valid
  channels only. F4 clears the 0xAA marker, then moves to the next valid channel (memory mode is
  left if none remain).
- Message box (0x0802f7bc): title rect (10,5)-(117,18), body rect (10,18)-(117,52), text
  centred, shown ~2 s.
- VER (0x0803198c): full screen, centred lines "-------VERSION-------", "V1.4", date, time,
  "Press any key to exit".

## Setting editor (0x080313a0) and its descriptors

Descriptor (11 words): min, max, label, printf format, value-name table, u32* / i16* / u8* var,
change fn, save fn (0x08032579 writes the block + CRC), apply fn. UI: label blinks @(3,31), value
right after it, labels QUIT · · SAVE (0x08038c8c). Tune encoder changes and applies live;
F1 restores the original, F4 / tune push saves if changed ("ERROR" / "CAN'T SAVE!" on failure).

| soft key | descriptor | label | range / names | storage |
|---|---|---|---|---|
| CWF | 0x08039030 | CW FLT: | 50Hz / 250Hz / 500Hz | settings +6 |
| CWT | 0x08039004 | CW TONE: | 500..1000 "%d Hz" | settings +0 |
| CWR | 0x0803905c | CW RATIO: | 2.0 2.5 3.0 3.5 4.0 | settings +4 |
| QSK long | 0x08039088 | QSK TIME: | 0..1000 "%d ms" | settings +2 |
| KS | 0x08038f80 | KEY SPEED: | 5..50 "%d WPM" | settings +0x0A |
| KM | 0x08038fac | M/L/R: | MAN / AUTO-L / AUTO-R | settings +0x0B |
| IMB | 0x08038fd8 | IAMBIC: | MODE-A / MODE-B | settings +0x0C |
| DIS | 0x080390b4 | DISP: | SCOPE+S / SCOPE / BIG SCOPE:) | settings +9 |
| MG | 0x08038f54 | MIC GAIN: | 0..30 (= codec MICPGA 0x3B, 0.5 dB steps → 0..15 dB) | param2 +0 |
| TXP | 0x080390e0 | TX POWER: | LOW / MID / HIGH | work params +0x1A (saved at power-off) |

## RX demodulation per mode (radio_trx_task, DMA ISR)

LO = dial + offset: CW −tone, CWR +tone, AM +100 Hz (keeps the carrier out of the DC block),
else 0. CW filter = band-pass at the tone, width 50/250/500 Hz. AM = envelope at 48 kHz, DC removal (y += dx, y *= 0.9995) and ×2, no pre-AGC band-pass.
Power-off (0x08029d6c) saves FM channels, work params (+CRC) and band slots.
Power key (TIM2 1 ms tick 0x08030414 + 0x08029d6c, polled from the UI loop): after boot the key
must be released for 10 ms first; then a press shorter than 2000 ticks (0x20000774) toggles the
backlight on release (flags ^= 0x100, PC11, beep 523.3 Hz 100 ms if beep on); 2000 ticks held ->
power-off while still held: save FM channels / work params (if CRC changed) / band slots, mute FM,
50 ms, HP gain mute, PA15 PC10 PA8 = 1, PB1 PC5 PC9 = 0, PA5 bank shift register = 0, beep
392/329.6/261.6 Hz x 100 ms (if beep on), power-off bitmap 0x080397a0 for 2 s, PC11 = 0, IRQs off,
PC7 = 0, wait for key release, SYSRESETREQ. Beep sequences: {u32 freq x10, u32 ms}..., -1 end.

## WFM (0x08033990)

FM chip on I2C1 @0x60 in TEA5767 mode (driver 0x080308dc..0x08030964). Register image 0x20000d18
`{MUTE|PLL[13:8], PLL[7:0], 0x18 (HLSI, mono), 0x10 (XTAL 32.768 kHz), 0x00}`, ref at +0x0C = 32768;
PLL = round(4 * (f + 225 kHz) / 32768). Read: 5 bytes, byte 4 bits 7..4 = level. Boot writes the
image muted (+500 ms delay).

- Entry: HPL/HPR muted (0x0802c228(3, -7)), FM channels read from EEPROM 0x0800 (error -> zeros),
  PA2 latch = 1<<6 (0x0802b394(6, 0)), freq clamped to 87..108 MHz (else 87.0), unmute, tune,
  work params flags bit5 set (ui_task re-enters WFM at boot while it is set).
- Screen: font8 "FM RADIO" @(0,0), labels "QUIT   <     >" @(0,55); frequency (0x0802fca4):
  font 0x0804e1a0 (24x32 mono via GUI_FONT_TRANSINFO) MHz `%3d` @(0,15), 100 kHz digit @(88,15),
  font16 "M" @(116,35), point = FillCircle(79,44,2). "SEEK" @(54,0) while seeking.
- Keys: F1 quit (mute, save channels, HP 0x1c, HF filters back), F2/F3 seek down/up (100 kHz steps,
  50 ms settle, level > 9 stops and marks 0xAA, else marks 0; stops at the band edge), BAND-/BAND+
  previous/next 0xAA channel, tune encoder +-100 kHz, volume encoder as usual (volume 0 mutes the
  chip instead of HPL), volume push PB0. No TX.
- Power-off saves the FM channels (always) and mutes the chip.

## RF verification (open firmware, 2026-10-10)

Test: carrier 7.011 MHz, radio at 7.010.000 USB (1 kHz tone). Console `iqfft` / `sig`:
- carrier at −984 Hz in the raw complex baseband (I = L, Q = R; FFT bin 47 Hz) -> LO exact,
  spectrum sign inverted (the demodulator's Hilbert sign compensates; USB is correct).
- raw I/Q image rejection 43..50 dB (hardware balance) before the adaptive correction.
- opposite-sideband suppression (USB −49 dBFS vs LSB −81 dBFS) ≥ 31 dB, possibly noise-limited;
  was 20 dB with a 65-tap Hilbert at 48 kHz.
- No stored RX calibration exists in stock: VersaClock reference is nominal 26 MHz, RX I/Q balance
  is adaptive in the DSP (implemented). Stored calibration = TX IQ only (settings +0x10.., per band).
- DSP interrupt load 54 % at 48 kHz (255-tap Hilbert, 63-tap LPF, 4-accumulator FIRs). Stock
  decimates to 12 kHz for the SSB path; do the same before adding TX.

## TX (verified with the open firmware, 2026-10-10, dummy load 50 Ω, 15 V supply)

Switching as stock radio_trx_task (PB1, codec TX routing, PA8, PC5, PC10, PA15), DSP output
I -> left / Q -> right, plus stock DC offsets (settings +0x10/+0x14, x256 int32 units).
Carrier = stock CW level I = Q = 0.045 FS x power factor (0.2 / 0.4 / 1.0):
LOW 7.0 Vrms (~1 W), MID 12.7 Vrms (~3.2 W), HIGH 15.2 Vrms (~4.6 W, PA limit; stock AM HIGH ~15 V).
Stock snapshot during AM TX (LOW): I/Q DMA samples = 0.005 FS carrier + DC offsets, GPIO identical.
Stock AM TX (DMA ISR, mode 7, 48 kHz, no decimation): mic sample -> 4-biquad band-pass
(0x0803873c, +5.5 dB, ~50 Hz..5 kHz) -> x 0.476 (table +0x60) + 0.5 -> 5-biquad low-pass
(0x08038914, +6 dB, cut ~4.5 kHz) -> x 0.05 x 0.495 -> I = Q, x power. No limiter / ALC:
modulation = 1.8 x the raw mic sample (100 % at mic 0.28 FS), carrier 0.0248 FS x power.
Stock AM uses the same power factors as CW (0.2 / 0.4 / 1.0), so on HIGH the carrier alone is near
PA saturation and upward modulation is lost (open firmware, 1 kHz AM test tone at 50 %: loud on LOW,
almost inaudible on HIGH). Own: AM drive 0.2 / 0.3 / 0.4.
Shift registers (filters) need slow bit-banging (~1 µs per phase).

## CAT (CI-V)

- USART1 19200 8N1 (init 0x08026788 at the end of boot), RX DMA2 S2 circular 100 B, TX DMA2 S7.
  Parser 0x08021f1c on the struct at 0x20000408 {getc, send, ready, ...}, polled from
  radio_trx_task (0x08029f90; main loop and the TX loop with delay_ms(1)).
- Frame `FE FE to from cmd data.. FD`; the `to` address is not checked; reply `FE FE from to ..`
  (no echo). Byte +0x42 of the struct (0x2000044a) = link idle: set after 100000 polls without a
  byte, cleared by a complete frame; idle clears the CAT PTT request 0x20000318. Shown as icon
  0x080397f0 (6x8 "C") at (75,1) while not idle (0x08033400).
- Handler 0x0802eb00 returns 1 = FA, 2 = FB, 3 = data (`cmd` + reply bytes):

| cmd | stock behaviour |
|---|---|
| 00 03 05 | no data: freq (5 BCD, LSB first) of the current VFO/memory; 5 bytes: set (clamped 550 kHz..30 MHz) |
| 01 04 06 | no data: [mode, filter] (LSB/L-D 0, USB/U-D 1, AM 2, CW 3, CWR 7, NFM 5; filter = CW: CWF+1, else 1); 2 bytes: set (0/1 -> L-D/U-D if the data flag is on, 2 AM, 3 CW, 5 NFM, 7 CWR, other -> AM) |
| 02 | `550000 - 30000000` (5 BCD, 0x2D, 5 BCD) |
| 07 | none: VFO mode; 00 A, 01 B, A0 A=B (0x0803396c), B0 toggle |
| 0F | split = work flags bit2: read / set 0/1 |
| 11 | VFO flags bit1 (read), set clears it |
| 14 | 01: AF = volume x 255/46 (set -> 0x2000033c, applied by the main loop); other subs < 0x1A stored in 0x20002518 only |
| 15 | 02: S-meter = (dBm + 123) x 2.55, dBm (0x2000035c) clamped -123..-23; other subs 0 |
| 16 | 02 preamp (VFO flags bit0), 12 AGC read only (LSB/USB 2, CW/CWR 1, else 3), 50 lock (work flags bit1) |
| 19 | 00 -> 0x70 |
| 1A | 03: filter width in *binary* (CW 0/4/9, LSB/USB 0x1F, L-D/U-D 0x22, else 0x2C); 06: data flag 0x2000037d bit0 (`[06 flag 00]`, set with 3 bytes) |
| 1C | 00: read TX state 0x20000319 / set CAT PTT 0x20000318 |
| 1D | 01: FB; 19: `19 01 06`; `02 01 x`: work flags bit7 -> PB0 |
| 21 | 00 RIT offset (2 BCD + sign), 01, 02 flags: stored only (0x20000324/312/313) |
| 25 | `[sel]`: freq of selected (00) / other VFO, reply byte 0 = current VFO number (bug); 6 bytes: write the VFO array only |
| 26 | `[sel]`: `[cur vfo, mode, data flag, 1]`; 4 bytes: set mode of that VFO |

- Data flag (0x2000037d bit0) is kept in sync in radio_trx_task: a mode change sets it to
  "mode is L-D/U-D"; a flag change (1A 06) converts LSB<->L-D, USB<->U-D.
- Own implementation: `src/cat.c` (deviations listed there).

## SYSTEM CAL / TX IQ CAL (0x080330b0 / 0x08032e00)

- Entry: only via CAT. CI-V `1D 01` (handler 0x0802eb00) sets bit0 of the CAT struct flags; the
  parser (0x08021f1c) turns it into 0x2000044b = 1, and ui_task (end of 0x080341cc) then calls
  0x080330b0 with the item table 0x08038c2c: {" 1.TX IQ CAL", 0x08032e01}, {" 2.EXIT", 0x08033231}.
- SYSTEM CAL list: font8 "-----SYSTEM CAL-----" @(3,0), items from y 9 in 8 px steps, ">" @(0,y)
  on the selected one, tune encoder = cursor, TUNE push = run the item, F1 = exit (redraws the main
  screen). Labels 0x08038cc8 {QUIT, " ", " ", " "}.
- TX IQ CAL (0x08032e00): "----- TXIQ CAL -----" @(3,0); " TXI:" value @(0,9), " TXQ:" @(0,18),
  ">" on the selected row; "MAG:x<step>" @(0,36); "RX" / "TX" @(0,45). Labels 0x08038d7c
  {QUIT, TX, CLR, SAVE}. Values = settings +0x10 / +0x14 (0x20001fd4/8), range -1000000..1000000,
  written live into the DSP (0x10000018/1c, added << 8 to the int32 I/Q output samples).
  Keys: TUNE push toggles I/Q; BAND- / BAND+ = MAG index 0..3 (step table 0x08038f2c: 1, 10, 100,
  1000; starts at 2); F1 QUIT (no save; the edited values stay live in the DSP until power-off);
  F2 toggles 0x2000031b = TX request without the microphone routing (radio_trx_task skips
  0x0802c2b0(1)), i.e. only the carrier leakage is transmitted; F3 CLR (selected value = 0, also in
  the settings copy); F4 SAVE (settings block + CRC, "ERROR / CAN'T SAVE!" for 5 s on failure).
  The radio's factory values: TXI -48500, TXQ -26200.
- Per-band TX IQ table (settings +0x18, 10 x {i16 amp, i16 phase}, band limits 0x08038bb8:
  5.5 (sic) / 2 / 4 / 7 / 10 / 14 / 18 / 21 / 24 / 28 MHz): no UI writes it (defaults 0). On a band
  change 0x08033400 calls 0x0802e994(amp, phase): 0x1000006c = 1 + amp x 5e-5,
  0x10000064 = sin(phase x 0.005 deg), 0x10000068 = 1/cos - 1 (0 if |phase| >= 60 deg). Nothing reads
  0x10000064..6c: the correction is dead code in V1.4b01.
- Open firmware: SETTINGS > CAL > TX IQ CAL (CAT `1D 01` opens SETTINGS > CAL) with the stock DC rows
  and the per-band AMP / PHASE rows actually applied (I x (1 + amp x 5e-5), Q x 1/cos + I x sin).

## CSN: call sign editor (0x0803186c -> editor 0x08033dec)

Screen "CALL SIGN EDITOR" (font8, centred, y 0), rules at y 9 and 53, labels QUIT (0,55) / BACK (51,55)
/ SAVE (104,55) in font8, the text so far in font16 centred in (0,10)-(127,30). Below, at y 39, a strip
of 10 consecutive ASCII characters (0x20 + (base + i) % 95) at x = 4 + 12 i, initial base 'A'; the
cursor (2-pixel underline at y 47/48, x 3+12c .. 9+12c, blinks ~440 ms) moves with the tune dial and
scrolls the strip at the ends. Keys: tune push (0x202) appends the character (max 15), F2 / F3
(0x102 / 0x104) delete the last one, F4 (0x201) saves, F1 (0x101) quits. Save writes the whole
0x12-byte record to EEPROM 0x0500 (`0xAA, len, chars`; empty text = `0, 0`), on failure the box
"ERROR / CAN'T SAVE!" for 5 s. Stock stores `base + cursor + 0x20` without the modulo (wrong characters
in the wrapped part of the strip); ours stores what is shown.

## CW keyer and CW TX (stock 0x080272b4 tick, 0x0802221c key out, 0x080221fc paddles)

A generic keyer state machine (struct at 0x20002008) is ticked from the TIM2 1 ms interrupt. Fields:
+4 paddle callback (PA6 low = bit0 dot, PA7 low = bit1 dash), +0xC KS WPM (5..100), +0x10 KM (0 MAN,
1 AUTO-L, 2 AUTO-R = paddle bits swapped), +0x14 IMB (1 = mode B), +0x34 QSK time, +0x40 dot ms =
1200 / WPM (float), +0x44 dash ratio (CWR "2.0".."4.0" parsed with atof, clamped 2..4).
- MAN: either paddle = key down.
- Iambic: at the end of an element (on-time and gap both 0) the next one is chosen: remembered
  opposite element first, else both paddles -> the opposite of the last element (dot first), one
  paddle -> its element. Dot: on `dot - 5` ms, gap `dot + 5` ms; dash: on `dash - 5`, gap `dot + 5`
  (dash = int(dot x ratio)). Mode B: while an element runs, the opposite paddle sets a memory
  (the next element is the opposite one); mode A samples the paddles only between elements.
- Hang: every key-on tick sets hang = QSK time and +0x30 "active" = 1; when idle with no paddle the
  hang counts down, then 10 more ms, then active = 0. active (0x2000032f) requests TX in
  radio_trx_task together with PTT / CAT PTT.
- Key out (0x0802221c): CCM 0x10000044 = 0.5 / 0; on the edges the sidetone (TIM1 CH4 beeper at the
  CWT frequency, 0x08027504) starts / stops.
- DSP (modes 4 / 5, 48 kHz): envelope e += / -= 1/960 per sample (0 .. 0.5, 10 ms ramps), I = Q =
  e x 0.09 x power factor (+ DC offsets); LO = f in TX (CW -tone / CWR +tone only in RX).
- PTT in CW mode: T/R relays only (no carrier without the key).
- Split (SPL, page 4 F1, 0x08032c94): toggles work flags bit2 (icon "SPL" at (86,1); CAT 0F sets it).
  radio_trx_task (0x0802a136): with split and in memory mode TX is refused (error beep 523 Hz 100 ms,
  0x08038a08), also when the other VFO is outside the TX band table. While TX is active the LO / band
  filters use the other VFO's frequency (0x0802a2b6; the mode used for the DSP stays the current
  VFO's) and the main screen swaps the VFOs (0x08033400): label, mode, preamp, frequency of the TX
  VFO on the main line, the RX VFO on the second line; swapped back on RX.
- QSK flag = work params flags bit3 (saved with them): short press of the QSK soft key toggles it
  (0x08032c0c), icon "QSK" at (109,1) while set. In the keyer tick (0x0802740c) the flag is copied
  to +0x24; while it is 0 `active` is forced to 0, i.e. the key never requests TX itself: with QSK
  off TX needs PTT (the key then only keys the carrier). Long press = QSK TIME (hang).
