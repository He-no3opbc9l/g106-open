# G106 open firmware

[![Build firmware](https://github.com/He-no3opbc9l/g106-open/actions/workflows/build.yml/badge.svg)](https://github.com/He-no3opbc9l/g106-open/actions/workflows/build.yml)

**English** | [Русский](README.ru.md)

Open-source firmware for the **G106** HF QRP transceiver, written from scratch in bare-metal C
(no HAL, no RTOS). It replaces the stock application (V1.4b01) and runs behind the **untouched stock
XG bootloader**, so it is flashed the same way as an official update and the stock firmware can be
put back at any time.

The first goal is faithful stock behaviour: same screen, keys, soft-key pages, setting editor,
memories and EEPROM format (byte-compatible with stock, including CRCs, so settings survive switching
between stock and this firmware). On top of that come improvements to the DSP and the UI.

> **Warning.** This is experimental firmware for a radio transmitter. Do TX tests into a dummy load.
> There are **no TX band limits**: you are responsible for transmitting only where your licence
> allows. Back up your radio (EEPROM calibration, ideally the whole flash) before flashing.
> No warranty of any kind. Not affiliated with or endorsed by the radio manufacturer.

## Features

Working on the radio:

- Main screen with the stock UI (original fonts and icons are taken from the official firmware at
  build time, see below; the boot logo and power-off screen are our own), tuning, bands, V/M, A/B, memories (MW / MC), VER, message boxes.
- Stock power key behaviour (short press: backlight, hold 2 s: power-off screen and save).
- Setting editor: CWF CWT CWR QSK KS KM IMB DIS MG TXP; call sign editor (CSN) as stock, the call
  sign is shown on the boot screen (default "G106 OPEN").
- RX: SSB, CW, AM, NFM. 48 kHz I/Q → DC block → adaptive I/Q balance → 16 kHz SSB phasing demodulator
  (191-tap Hilbert), CW biquad filters, AM envelope, NFM discriminator with de-emphasis and squelch, band-pass filter, dB AGC with look-ahead.
- 1024-point FFT spectrum scope with zoom and an FFT-based S-meter.
- TX: SSB, AM and NFM voice via PTT, data input for L-D / U-D (digital modes).
- CW TX as stock: iambic / straight keyer on the key jack (PA6 dot, PA7 dash; KS speed, KM MAN /
  AUTO-L / AUTO-R, IMB mode A / B, CWR dash ratio), sidetone at the CWT pitch, 10 ms keying
  ramps, QSK hang time (short press of the QSK soft key switches key-controlled TX on / off, as
  stock: with QSK off the key only works while PTT is held). Not tested with a real key yet.
- WFM broadcast receiver (built-in FM chip): seek and channels.
- CAT: Icom CI-V protocol on USART1 like stock (19200 8N1), see [CAT](#cat).

Deliberate differences from stock:

| | |
|---|---|
| FILT | RX/TX audio filter, own width per mode group: SSB, DATA (L-D/U-D), AM 2.0–6.0 kHz, NFM 2.0–3.0 kHz; low cut 20 Hz (NFM 300 Hz); in CW it opens CWF (soft-key page 6) |
| ZOOM | spectrum zoom 1.0–3.0× (page 6) |
| AGC | OFF / SOFT / MED / HARD (HARD = stock, OFF = fixed gain) (page 6) |
| MIC GAIN | 0..60 = 0..+30 dB in 0.5 dB steps (stock: 0..30) |
| Mic EQ, COMP | 3-band mic equaliser and compressor (page 7); data modes bypass them |
| Preamp | global, not per VFO/band |
| TX switching | 10 ms relay delay + 10 ms ramps, no band limits |
| AM TX | mic +12 dB before the limiter (≤ 90 % modulation) |
| NFM | enabled in the MODE cycle (stock lists it but disables it): ±2.5 kHz deviation, 6 dB/oct pre-/de-emphasis; SQL = squelch OFF / 1..30 by the S-meter level (2 dB per step), its threshold is shown as an arrow over the S-meter (page 6) |

Not done yet (stock parity): split, TX I/Q
correction and calibration menus, power/SWR metering, factory reset.

## Download

Ready-made images are built by GitHub Actions: tagged versions on the
[Releases](https://github.com/He-no3opbc9l/g106-open/releases) page, every commit under
[Actions](https://github.com/He-no3opbc9l/g106-open/actions) (artifacts). The release zip
holds the images; flash
`g106_open-<version>.xgf` like an official update (see [Flashing](#flashing)); the `.bin` is for SWD.
The easiest way is the [web flasher](https://he-no3opbc9l.github.io/xg-flasher/): it lists the releases itself.

## Building

Requirements: `arm-none-eabi-gcc` (tested with 14.2), GNU make, Python 3, `curl`, `unzip`
(+ `pyserial` for flashing), optionally OpenOCD and an ST-Link for SWD.

```
make
```

The build needs the official firmware **G106 V1.4b01** (`stock/G106Fw-V1.4b01.xgf`): on the first
run `make` downloads the official update archive and checks the file's sha256 (offline: put the
file into `stock/` by hand). It is not part of this repository; the build decrypts it and extracts
the original UI fonts and icons from it (`tools/extract_stock_gfx.py`), so no stock graphics are
stored in the sources.

Output: `build/g106_open.elf`, `.bin` (raw image for `0x08020000`) and `.xgf` (encrypted image for
the bootloader). `make stock` additionally produces `stock/G106Fw-V1.4b01.bin` (decrypted stock app)
and `stock/G106Fw-V1.4b01.txall.bin` (stock app with the TX band limit removed).

`.xgf` files are plain AES-256-ECB; `tools/xgf.py enc|dec` uses the key in `tools/aes_key.hex`.

## Flashing

### Web flasher (browser, only a USB-UART adapter)

**https://he-no3opbc9l.github.io/xg-flasher/** — Chrome / Edge / Opera on a computer (Web Serial). Choose a release (or your own
`.xgf`, a `.zip` holding one, e.g. the official Xiegu archive to go back to stock, or a raw `.bin`),
choose the adapter's port, press *Flash* and switch the radio on holding the **volume** knob. The
file is decrypted and checked before sending; the bootloader itself is never touched. Sources:
[xg-flasher](https://github.com/He-no3opbc9l/xg-flasher).

### Via the stock bootloader (IAP, only a USB-UART adapter)

USART1 of the radio (PA9 TX / PA10 RX, 3.3 V, 115200 8N1) to a USB-UART adapter.

```
make flash-iap PORT=/dev/ttyUSB0
```

Then switch the radio on while holding the **volume** knob: the bootloader menu
(`XG Bootloader(v2.0)`) appears, the script selects the update and sends the image via XMODEM-1K.
After `Complete!` switch the radio off and on normally. If the menu is already on screen, flashing
starts immediately.

Note: the XG bootloader sets flash read protection (RDP level 1) after an IAP update. That does not
matter for normal use, but SWD (below) does not work after that until RDP is cleared.

### Via SWD (ST-Link, for development)

Requires RDP level 0 (`mdw 0x40023C14` in OpenOCD: byte 1 = `0xAA`). Clearing RDP1 means a mass
erase of the whole flash, **including the bootloader**: only do it if you have a full flash dump to
restore.

```
make flash-swd
```

The script halts the CPU (**never resets it**: any MCU reset drops the soft-power latch and the radio
switches off), writes and verifies the app, then switches the radio off via the power latch. Switch
it on with the power key. The first write attempt sometimes times out; the script retries in a fresh
OpenOCD session.

### Back to stock

- IAP: `python3 -I tools/flash_iap.py /dev/ttyUSB0 stock/G106Fw-V1.4b01.xgf`
- SWD: `make restore-stock-swd` (writes the TX-unlocked stock app).

The EEPROM is shared and stays in stock format, so stock starts with your settings.

## CAT

Same as stock: Icom CI-V on USART1 (the port used for IAP), **19200 8N1**, no echo, any CI-V
address is accepted, the transceiver ID (`19 00`) is `0x70`. In programs choose an Icom CI-V rig
(e.g. IC-7000 / Xiegu profiles with address 0x70), 19200 baud, PTT via CAT. A "C" icon at the
top of the screen shows that CAT frames are coming in.

Supported: frequency and mode (`03`/`05`, `04`/`06`, also of VFO A/B via `25`/`26`), data mode
L-D/U-D (`1A 06`), VFO A/B, A=B, swap (`07`), PTT (`1C 00`), S-meter (`15 02`), volume (`14 01`),
TX power LOW/MID/HIGH (`14 0A`), preamp (`16 02`), lock (`16 50`), filter width (`1A 03`), band
edges (`02`). Split is not implemented yet. CAT PTT is dropped if no frame arrives for 60 s. The
full list with deviations from stock is at the top of `src/cat.c`.

## Repository layout

| path | |
|---|---|
| `src/` | firmware sources (`radio.c` UI, `audio.c` DSP, `spectrum.c` scope, `settings.c` EEPROM, drivers) |
| `ld/` | linker script (app at `0x08020000`) |
| `tools/` | `xgf.py` (encrypt/decrypt), `flash_iap.py`, `flash_swd.*`, `extract_stock_gfx.py`, `patch_txall.py`, Ghidra scripts |
| `docs/hardware.md` | hardware map: pins, peripherals, chips, bootloader contract |
| `docs/stock_spec.md` | reverse-engineered stock behaviour (EEPROM layout, keys, UI, DSP, TX) |

## License

[GPL-3.0](LICENSE). The stock firmware, fonts and graphics are property of their owner and are not
distributed here.
