/* Linker script for an RP2350-class Cortex-M33 part (Raspberry Pi Pico 2).
 *
 * Deliberately minimal. This is a MEASUREMENT harness, not a bootable image: there is no second-stage bootloader,
 * no XIP setup and no clock configuration, because none of those are quorum's code and including them would
 * contaminate the footprint numbers with somebody else's bytes.
 *
 * What it does provide is a real link, with real relocation and real section placement, so `size` reports what the
 * part would actually have to hold rather than a sum of object files.
 */

MEMORY
{
    /* Pico 2 carries 4 MB of QSPI flash, mapped for execute-in-place at 0x10000000. */
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4M
    /* RP2350: 520 KB of on-chip SRAM. This is the number the whole "runs on a microcontroller" claim rests on. */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 520K
}

ENTRY(reset_handler)

SECTIONS
{
    .vectors : {
        KEEP(*(.vectors))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
        __data_load = .;
    } > FLASH

    .data : AT(__data_load) {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* The stack is what a bounded, no-heap, no-recursion design buys: a number that can be STATED rather than
     * estimated. The compiler's own frame accounting (-fstack-usage) puts the sum of every frame in the image at
     * 332 bytes, and because there is no recursion anywhere that sum is an absolute ceiling, not a sample.
     *
     * 2 KB reserved: 6x headroom over the measured ceiling, with the surplus left deliberately for the interrupt
     * handlers a real integration will add. The first draft reserved 8 KB, which was 25x the real requirement and
     * cost more RAM than the entire reasoning engine. */
    .stack (NOLOAD) : {
        . = ALIGN(8);
        __stack_bottom = .;
        . += 2K;
        . = ALIGN(8);
        __stack_top = .;
    } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
