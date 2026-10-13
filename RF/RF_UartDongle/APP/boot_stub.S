/* Keep a small, permanently erasable reset stub at the start of Host Flash.
 * Erasing this block makes CH570 ROM ISP take ownership after reset while the
 * application itself remains outside the ISP hand-off area. */
    .section .boot,"ax",@progbits
    .global _boot_start
    .align 1
_boot_start:
    j _start
