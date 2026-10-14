/* fiber_aarch64.S - fiber context switch for aarch64 (AAPCS64, Linux ELF). See fiber.h for what is saved and why.
 *
 * struct plat_fiber starts with uint64_t regs[22]:
 *   [0..9] x19..x28  [10] x29  [11] x30  [12] sp  [13..20] d8..d15  [21] fpcr
 */
#if defined(__aarch64__)

#define BTI_C hint #34          /* bti c: landing pad for blr; NOP without FEAT_BTI */

        .text

/* void plat_fiber_switch_raw(plat_fiber *from, plat_fiber *to) */
        .globl  plat_fiber_switch_raw
        .type   plat_fiber_switch_raw, %function
        .balign 16
plat_fiber_switch_raw:
        .cfi_startproc
        BTI_C
        stp     x19, x20, [x0, #0]
        stp     x21, x22, [x0, #16]
        stp     x23, x24, [x0, #32]
        stp     x25, x26, [x0, #48]
        stp     x27, x28, [x0, #64]
        stp     x29, x30, [x0, #80]
        mov     x9, sp
        str     x9, [x0, #96]
        stp     d8,  d9,  [x0, #104]
        stp     d10, d11, [x0, #120]
        stp     d12, d13, [x0, #136]
        stp     d14, d15, [x0, #152]
        mrs     x10, fpcr
        str     x10, [x0, #168]

        ldp     x19, x20, [x1, #0]
        ldp     x21, x22, [x1, #16]
        ldp     x23, x24, [x1, #32]
        ldp     x25, x26, [x1, #48]
        ldp     x27, x28, [x1, #64]
        ldp     x29, x30, [x1, #80]
        ldr     x9, [x1, #96]
        mov     sp, x9
        ldp     d8,  d9,  [x1, #104]
        ldp     d10, d11, [x1, #120]
        ldp     d12, d13, [x1, #136]
        ldp     d14, d15, [x1, #152]
        ldr     x11, [x1, #168]
        cmp     x10, x11
        b.eq    1f
        msr     fpcr, x11
1:      ret                     /* to the saved lr: the switch call site, or plat_fiber_trampoline */
        .cfi_endproc
        .size   plat_fiber_switch_raw, .-plat_fiber_switch_raw

/* First code of a new fiber, entered by the `ret` above with x19 = fn, x20 = arg, x21 = the fiber, x29 = 0. */
        .globl  plat_fiber_trampoline
        .type   plat_fiber_trampoline, %function
        .balign 16
plat_fiber_trampoline:
        .cfi_startproc
        .cfi_undefined x30      /* outermost frame for unwinders */
        BTI_C
        mov     x29, #0
        mov     x30, #0
        mov     x0, x20
        blr     x19             /* fn(arg): must not return */
        mov     x0, x21
        bl      plat_fiber_entry_returned       /* noreturn: reports and aborts */
        brk     #0x3e8
        .cfi_endproc
        .size   plat_fiber_trampoline, .-plat_fiber_trampoline

        .section .note.GNU-stack, "", %progbits

/* GNU property: GNU_PROPERTY_AARCH64_FEATURE_1_AND = BTI | PAC, so BTI-enabled links stay BTI. */
        .pushsection .note.gnu.property, "a"
        .balign 8
        .long   4               /* name size */
        .long   16              /* descriptor size */
        .long   5               /* NT_GNU_PROPERTY_TYPE_0 */
        .asciz  "GNU"
        .long   0xc0000000      /* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
        .long   4
        .long   3               /* BTI | PAC */
        .long   0
        .popsection

#endif
