# Write the app at 0x08020000 over SWD without any MCU reset (a reset would drop the PC7
# soft-power latch uncontrolled), then switch the radio off cleanly via PC7 like stock does.
# Expects $IMG. Starting the new image in place ("hot swap") proved unreliable, so the user
# powers the radio on again with the power key.
#
# Before writing: mask IRQs, stop SysTick, all DMA streams (they write into SRAM where openocd
# places the flash algorithm), I2S and the beeper.

reset_config none
init
halt
reg primask 1
mww 0xE000E010 0
foreach base {0x40026000 0x40026400} {
    for {set s 0} {$s < 8} {incr s} {
        catch {mww [expr {$base + 0x10 + 0x18 * $s}] 0}
    }
}
catch {mww 0x40010044 0}
catch {mww 0x4000381C 0}
catch {mww 0x4000341C 0}

# One attempt per openocd session: after a failed write openocd keeps the algorithm state
# and refuses to retry, so flash_swd.sh re-runs this script in a fresh process.
if {[catch {flash write_image erase $IMG 0x08020000 bin} err]} {
    echo "FLASH WRITE FAILED: $err"
    catch {mww 0x40023C0C 0x000000F3}
    shutdown error
    return
}
verify_image $IMG 0x08020000 bin
mww 0x40020818 0x00800000
echo "flashed and verified; radio powered off (PC7 = 0) - switch it on with the power key"
shutdown
