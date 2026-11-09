#!/bin/sh
# Write an app image to 0x08020000 over SWD, verify, and power the radio off (see flash_swd.tcl).
# The first write over a running audio build sometimes times out; a fresh openocd session has
# always succeeded, so the write is retried up to 3 times.
IMG=${1:?usage: flash_swd.sh IMAGE.bin}
[ -f "$IMG" ] || { echo "no such file: $IMG" >&2; exit 1; }
for try in 1 2 3; do
    if openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "set IMG {$IMG}" \
            -f "$(dirname "$0")/flash_swd.tcl"; then
        exit 0
    fi
    echo "write attempt $try failed, retrying in a new session"
done
exit 1
