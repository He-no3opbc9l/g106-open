#!/usr/bin/env python3
"""One-shot IAP flash through the stock XG bootloader (USART1, XMODEM-1K).

usage: python3 -I tools/flash_iap.py PORT FILE.xgf [timeout_s]

Works whether the bootloader menu is already on screen (sends "1" at once) or the radio is still
off / running (waits until it is powered on holding the volume button, then sends "1").
"""
import sys
import time

import serial

SOH, STX, EOT, ACK, NAK, CAN = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def show(data):
    sys.stdout.write(data.decode("latin-1").replace("\r", "\n").replace("\x00", ""))
    sys.stdout.flush()


def send_block(ser, num, chunk):
    chunk = chunk.ljust(1024, b"\x1a")
    pkt = bytes([STX, num & 0xFF, 0xFF - (num & 0xFF)]) + chunk + crc16(chunk).to_bytes(2, "big")
    for _ in range(10):
        ser.write(pkt)
        ser.flush()
        r = ser.read(1)
        if r and r[0] == ACK:
            return True
        if r and r[0] == CAN:
            return False
    return False


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, path = sys.argv[1], sys.argv[2]
    limit = float(sys.argv[3]) if len(sys.argv) > 3 else 300
    fw = open(path, "rb").read()
    total = (len(fw) + 1023) // 1024
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()

    print("%s: %d bytes. If the radio is not in the IAP menu yet: power it on holding the volume "
          "button." % (path, len(fw)), flush=True)
    ser.write(b"1")                     # menu already shown: starts the update right away
    buf = b""
    sent_at = time.time()
    end = time.time() + limit
    while True:
        if time.time() > end:
            print("\ntimeout: no XMODEM request from the bootloader, nothing was sent")
            return 2
        d = ser.read(256)
        if d:
            show(d)
            buf = (buf + d)[-512:]
        # checked on every pass: the menu may show up < 1 s after the last "1" and then go quiet
        low = buf.lower()
        if b"update fw" in low and time.time() - sent_at > 1.0 and b"erasing" not in low:
            time.sleep(0.3)
            ser.write(b"1")
            sent_at = time.time()
            buf = b""
        elif b"wait fw file" in low:
            if b"C" in buf[low.rindex(b"wait fw file") + 12:]:
                break                   # receiver asks for XMODEM-CRC with 'C'

    print("\nreceiver ready, sending %d blocks" % total, flush=True)
    time.sleep(0.2)
    ser.reset_input_buffer()
    for i in range(total):
        if not send_block(ser, i + 1, fw[i * 1024:(i + 1) * 1024]):
            print("block %d/%d FAILED" % (i + 1, total))
            return 3
        if i % 10 == 0 or i == total - 1:
            print("block %d/%d" % (i + 1, total), flush=True)
    for _ in range(10):
        ser.write(bytes([EOT]))
        r = ser.read(1)
        if r and r[0] == ACK:
            break
    else:
        print("no ACK for EOT")
        return 4
    time.sleep(1.5)
    tail = ser.read(4096)
    show(tail)
    if b"Complete" not in tail:
        print("\nWARNING: no 'Complete!' from the bootloader")
        return 5
    print("\nOK: power-cycle the radio normally.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
