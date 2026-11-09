/* G106 open app — runs behind the stock XG Bootloader v2.0.
 *
 * Bootloader contract (reversed from g106_bootloader.bin @ 0x0800164c):
 *   - vector table at 0x08020000
 *   - (initial SP & 0xFFFF0000) == 0x20000000  -> SP must be below 0x20010000
 *   - (Reset vector & 0xFFF80000) == 0x08000000 -> Reset_Handler below 0x08080000
 * So the stack sits at the BOTTOM of SRAM (like the stock app) and data/bss follow it.
 */
ENTRY(Reset_Handler)

STACK_SIZE = 8K;

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08020000, LENGTH = 896K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    CCM   (rw)  : ORIGIN = 0x10000000, LENGTH = 64K   /* no DMA access */
}

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text.Reset_Handler)
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        . += STACK_SIZE;
        . = ALIGN(8);
        _estack = .;
    } > RAM

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .ccm (NOLOAD) :
    {
        *(.ccm*)
    } > CCM

    _end = _ebss;
    end = _end;
}

ASSERT(_estack < 0x20010000, "initial SP must be below 0x20010000 (bootloader check)")
ASSERT(Reset_Handler < 0x08080000, "Reset_Handler must be below 0x08080000 (bootloader check)")
