/*
 * setjmp.S – setjmp/longjmp a játéknak (bionic jmp_buf: 64 long = 256 bájt)
 *
 * Nem a newlibé: az soft-float, nem menti a d8–d15-öt, a játék kódja viszont VFP-t használ
 * (AAPCS: d8–d15 hívott fél által mentett). A puffer saját elrendezése (a játék nem nézi bele):
 *   [0..31]   r4–r11
 *   [32..39]  sp, lr
 *   [40..103] d8–d15
 *   [104]     FPSCR
 * ARM-módú függvények; a játék PLT-je (ldr pc) és a Thumb-hívók blx-e is helyesen vált módot.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
    .syntax unified
    .arm
    .fpu neon-fp-armv8
    .text

    .global bionic_setjmp
    .type   bionic_setjmp, %function
    .align  2
bionic_setjmp:
    stmia   r0!, {r4-r11}
    mov     r2, sp
    stmia   r0!, {r2, lr}
    vstmia  r0!, {d8-d15}
    vmrs    r1, fpscr
    str     r1, [r0]
    mov     r0, #0
    bx      lr
    .size   bionic_setjmp, . - bionic_setjmp

    .global bionic_longjmp
    .type   bionic_longjmp, %function
    .align  2
bionic_longjmp:
    ldmia   r0!, {r4-r11}
    ldmia   r0!, {r2, lr}
    vldmia  r0!, {d8-d15}
    ldr     r3, [r0]
    vmsr    fpscr, r3
    mov     sp, r2
    // longjmp(buf, 0) → a setjmp 1-et ad vissza
    movs    r0, r1
    moveq   r0, #1
    bx      lr
    .size   bionic_longjmp, . - bionic_longjmp

    .section .note.GNU-stack, "", %progbits
