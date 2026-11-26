# G106 open firmware

[![Build firmware](https://github.com/He-no3opbc9l/g106-open/actions/workflows/build.yml/badge.svg)](https://github.com/He-no3opbc9l/g106-open/actions/workflows/build.yml)

**English** | [Русский](README.ru.md)

💬 [Telegram chat](https://t.me/xg_open) · ⚡ [Install the firmware in the browser](https://he-no3opbc9l.github.io/xg-flasher)

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
  build time, see below; the boot logo and power-off screen are our own), tuning, bands, V/M, A/B, memories (MW / MC), message boxes.
- Stock power key behaviour (short press: backlight, hold 2 s: power-off screen and save).
- Setting editor: CWF CWT CWR QSK KS KM IMB DIS MG TXP; call sign editor (CSN) as stock, the call
  sign is shown on the boot screen (default "G106 OPEN").
- RX: SSB, CW, AM, NFM. 48 kHz I/Q → DC block → adaptive I/Q balance → 16 kHz SSB phasing demodulator
  (191-tap Hilbert), CW biquad filters, AM envelope, NFM discriminator with de-emphasis and squelch, band-pass filter, dB AGC with look-ahead.
- 1024-point FFT spectrum scope with zoom and an FFT-based S-meter.
- TX: SSB, AM and NFM voice via PTT (front PTT and the rear-connector TTL PTT line), data input for L-D / U-D (digital modes).
- CW TX as stock: iambic / straight keyer on the key jack (PA6 dot, PA7 dash; KS speed, KM MAN /
  AUTO-L / AUTO-R, IMB mode A / B, CWR dash ratio), sidetone at the CWT pitch, 10 ms keying
  ramps, QSK hang time (short press of the QSK soft key switches key-controlled TX on / off, as
  stock: after power-up the key works right away, switching QSK off with the soft key makes it work only
  while PTT is held). Not tested with a real key yet.
  PTT / CAT PTT in CW / CWR only switch to TX (as stock); the carrier comes from the key.
- Split (SPL, page 4): RX on the current VFO, TX on the other one (frequency only), SPL icon, the
  TX VFO is shown on the main line while transmitting; CAT 0F. No TX in split + memory mode, as stock.
- WFM broadcast receiver (built-in FM chip): seek and channels.
- CW decoder (own, stock has none; SETTINGS > CW > DECODER, default off): in CW / CWR the received
  Morse is shown as text in place of the VFO B line, newest character on the right, with the
  estimated speed in WPM at the right edge (shown inverted while the decoder hears the key down -
  a tuning aid: tune the signal onto the CW tone, the decoder listens 100..200 Hz around it).
  Speed 5..50 WPM is learnt automatically (also Farnsworth spacing and hand keying), letters,
  digits, punctuation and the prosigns <AR> <KN> <SK> <AS> <HH> (`*` = unknown code).
  DECODER LEVEL: AUTO (threshold between noise and signal, nothing is decoded below ~10 dB SNR in
  the decoder bandwidth) or 1..20 dB over the noise floor for weak signals (too low a level
  decodes the noise). The decoder pauses while transmitting.
- Noise reduction (own, stock has none; SETTINGS > RX > NOISE REDUCER / NR LEVEL, default off;
  FILT long press on soft-key page 6 switches it on / off): spectral noise reduction of the
  received audio in SSB, CW, AM and NFM (not in the data modes L-D / U-D). The noise spectrum is
  learnt continuously, no setup; a Wiener-type gain with decision-directed SNR keeps the residual
  noise natural (no "musical" warbling). NR LEVEL 1..10 = noise attenuation 6..24 dB (default 5,
  14 dB). Signal loudness stays the same (the AGC still measures the unprocessed signal), only the
  noise gets quieter. Adds 24 ms of audio delay and ~7 % DSP load; "NR" is shown left of the
  frequency while it is active. Rising noise is learnt within 1..3 s; a steady tone held for
  seconds is slowly treated as noise. For weak CW a narrow CW filter (CWF) works better.
- CAT: Icom CI-V protocol on USART1 like stock (19200 8N1), see [CAT](#cat).

Deliberate differences from stock:

| | |
|---|---|
| FILT | RX/TX audio filter, own width per mode group: SSB, DATA (L-D/U-D), AM 2.0–6.0 kHz, NFM 2.0–3.0 kHz; low cut 20 Hz (NFM 300 Hz); in CW it opens CWF (soft-key page 6) |
| LOW CUT | common low cut (high-pass, 4th order) 10–400 Hz for all modes, RX and TX (default 20 Hz; NFM keeps its fixed 300 Hz cut below it) (SETTINGS > RX) |
| SCOPE ZOOM | spectrum zoom 1.0–3.0× (page 6, ZOOM soft key; also SETTINGS menu) |
| AGC | OFF / SOFT / MED / HARD (HARD = stock, OFF = fixed gain) (SETTINGS menu) |
| MIC GAIN | 0..60 = 0..+30 dB in 0.5 dB steps (stock: 0..30) |
| Mic EQ, COMP | 3-band mic equaliser and compressor (SETTINGS menu) for SSB, AM and NFM; data modes bypass them |
| SETTINGS | menu (SETT soft key, page 6) with categories RX, TX, CW, DISPLAY, SYSTEM, CAL: all own and advanced settings; PTT works inside it for live mic/TX adjustments |
| VER | own screen: firmware version, build date, uptime, DSP load, EEPROM / I2C state |
| CONTRAST | LCD contrast (SETTINGS menu) |
| TX IQ CAL | SETTINGS > CAL (stock: hidden SYSTEM CAL menu, CAT `1D 01`, which also opens it here): carrier null TXI / TXQ as stock, plus TX I/Q amplitude / phase per band (stock keeps this table but never applies it); F2 transmits without modulation (carrier rows) or a 1 kHz tone (AMP / PHASE rows), BAND-/BAND+ = step, F3 = clear, BACK asks "SAVE CHANGES?" (NO restores the old values) |
| FREQ CAL | reference crystal correction ±50 ppm in 0.01 ppm steps (SETTINGS > CAL; stock has none) |
| Preamp | global, not per VFO/band |
| TX switching | 10 ms relay delay + 10 ms ramps, no band limits |
| AM TX | own: the same voice processing as SSB (FILT AM, LOW CUT, mic EQ, COMP, limiter), modulation ≤ ~94 %, own carrier level (SETTINGS > TX > AM DRIVE 1..20, default 6): the stock AM (carrier ≈ 50 on this scale) saturates the PA and sounds hoarse; power LOW / MID / HIGH = −8 / −4 / 0 dB |
| NFM TX | speech compressor always on (≥ +20 dB) + limiter (±2.5 kHz) |
| NFM | enabled in the MODE cycle (stock lists it but disables it): ±2.5 kHz deviation, 6 dB/oct pre-/de-emphasis |
| Squelch | NFM and SSB (LSB/USB; not the data modes), own level per mode OFF / 1..30 by the S-meter level (2 dB per step), threshold shown as an arrow over the S-meter; SQL soft key (page 6) = level of the current mode, SQL long = squelch on/off; also SETTINGS > RX (SQUELCH, NFM SQUELCH, SSB SQUELCH). Hang time NFM 150 ms, SSB 500 ms |
| Start-up / LO | codec (TLV320) and VersaClock registers are read back after init and the init is repeated on a mismatch (up to 3 attempts); the LO divider is written in one burst (integer + fraction) and read back on every frequency change. Stock writes them blindly, in two transactions |

Not done yet (stock parity): power/SWR metering, factory reset.

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

`make test-cwdec` builds and runs a host test of the CW decoder (`tools/cwdec_test.c`, any C
compiler): a simulated receiver with keyed tones at 5..50 WPM, noise, fading, hand keying and
Farnsworth spacing; `SEED=n` varies the noise.

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

## Known issues

- **S9+40 noise floor after power-on.** Sometimes the radio shows a constant S9+40 noise right after
  switching on. If it happens, just power-cycle the radio. The cause is not fully investigated: a failed
  codec init at power-on is the main suspect (init is now read back and retried, see issue #6), so
  please report if it still happens in newer builds.

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
| `tools/` | `xgf.py` (encrypt/decrypt), `flash_iap.py`, `flash_swd.*`, `extract_stock_gfx.py`, `patch_txall.py`, `cwdec_test.c` / `nr_test.c` (host tests), `nr_bench.c` + `nr_cycles.py` (NR cycle estimate), Ghidra scripts |
| `docs/hardware.md` | hardware map: pins, peripherals, chips, bootloader contract |
| `docs/stock_spec.md` | reverse-engineered stock behaviour (EEPROM layout, keys, UI, DSP, TX) |

## License

[GPL-3.0](LICENSE). The stock firmware, fonts and graphics are property of their owner and are not
distributed here.
