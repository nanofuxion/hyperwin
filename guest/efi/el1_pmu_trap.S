// Counter-read compatibility for EL1t (kernel using SP0).
// Installed at KiKernelSp0ExceptionHandler before its first X18 clobber.
// Select the interrupted kernel stack explicitly, preserving X18 unchanged.
// Handles exactly MRS PMCCNTR_EL0; no writes to executable memory, no blanket
// suppression of unknown system registers. Other exceptions run the displaced
// MRS X18, SP_EL0 and continue through the original Windows handler.
    .text
    msr spsel, #0
    sub sp, sp, #0x20
    stp x15, x16, [sp]
    str x17, [sp, #0x10]
    mrs x16, esr_el1
    lsr x15, x16, #26
    cbz x15, Lundef
    ubfx x15, x16, #10, #22
    and x17, x16, #0x1f
    orr x15, x17, x15, lsl #10
    movz x17, #0xe41b
    movk x17, #0x6230, lsl #16
    cmp x15, x17
    b.ne Lpass
    ubfx x15, x16, #5, #5
    b Lemulate
Lundef:
    mrs x17, elr_el1
    ldr w17, [x17]                  // faulting kernel instruction
    movz w16, #0x9d00
    movk w16, #0xd53b, lsl #16      // mrs x0, PMCCNTR_EL0
    and w15, w17, #0xffffffe0
    cmp w15, w16
    b.ne Lpass
    and x15, x17, #0x1f
Lemulate:
    mrs x17, cntvct_el0
    nop                             // three slots trace.c fills in: they scale the timer's
    nop                             // count up to the pace of the real processor's clock
    nop
    adr x16, Ltable
    add x16, x16, x15, lsl #3
    br x16
    .macro SETREG n
    mov x\n, x17
    b Ldone
    .endm
Ltable:
    SETREG 0
    SETREG 1
    SETREG 2
    SETREG 3
    SETREG 4
    SETREG 5
    SETREG 6
    SETREG 7
    SETREG 8
    SETREG 9
    SETREG 10
    SETREG 11
    SETREG 12
    SETREG 13
    SETREG 14
    str x17, [sp]                   // x15 lives in the save area
    b Ldone
    str x17, [sp, #8]               // x16
    b Ldone
    str x17, [sp, #0x10]            // x17
    b Ldone
    SETREG 18
    SETREG 19
    SETREG 20
    SETREG 21
    SETREG 22
    SETREG 23
    SETREG 24
    SETREG 25
    SETREG 26
    SETREG 27
    SETREG 28
    SETREG 29
    SETREG 30
    nop                             // xzr: nothing to write
    b Ldone
Ldone:
    mrs x16, elr_el1
    add x16, x16, #4
    msr elr_el1, x16
    ldp x15, x16, [sp]
    ldr x17, [sp, #0x10]
    add sp, sp, #0x20
    msr spsel, #1
    eret
Lpass:
    ldp x15, x16, [sp]
    ldr x17, [sp, #0x10]
    add sp, sp, #0x20
    msr spsel, #1
    mrs x18, sp_el0                 // displaced instruction
    b .
