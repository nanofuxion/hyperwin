// Firmware-side single-step support (see trace.c).
    .text
    .global HwSyncEntry
// Software-step trap taken while the firmware's own vectors are still in force.
HwSyncEntry:
    ldp x16, x17, [sp], #16
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x0, sp
    mrs x1, elr_el1
    bl HwSyncC
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #272
    eret

    .global HwStartStep
// Enable software single-step for the caller: MDSCR_EL1.SS|KDE, OS lock cleared, then eret to
// the next instruction with PSTATE.SS = 1 so the very next instruction traps.
HwStartStep:
    msr S2_0_C1_C0_4, xzr
    mrs x1, mdscr_el1
    orr x1, x1, #1
    orr x1, x1, #0x2000
    msr mdscr_el1, x1
    isb
    mrs x4, SPSel
    and x4, x4, #1
    mov x3, #0xc4
    orr x3, x3, x4
    orr x3, x3, #0x200000
    adr x1, 1f
    msr elr_el1, x1
    msr spsr_el1, x3
    eret
1:
    ret
