/* Keep the reset entry in the first flash block so erasing that block hands
 * control to the CH570 ROM ISP. The application itself starts at 0x2000. */
    .section .boot,"ax",@progbits
    .global _boot_start
    .align 1
_boot_start:
    j _start
