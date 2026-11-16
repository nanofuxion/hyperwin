// Kernel fault recorder (diagnostic). trace.c copies this stub into slack space of an image
// that programs VBAR_EL1 and points the "sync, current EL, SPx" vector at it.
// BRK and SVC pass through to the original handler. Any other synchronous exception is
// recorded: the stub finds its own physical address with AT, turns the MMU off across an ERET
// to that address, stores ESR/ELR/FAR/LR at physical 0x13ef00000 (the app's shared window)
// and powers the VM off through PSCI. The host app logs the record (tag HyperWinDbg).
// The last two words are filled in by trace.c: the displaced instruction and the branch back.
    .text
    stp x16, x17, [sp, #-16]!
    mrs x16, esr_el1
    lsr x16, x16, #26
    cmp x16, #0x3c
    b.eq Lpass
    cmp x16, #0x15
    b.eq Lpass
    mrs x10, esr_el1
    mrs x11, elr_el1
    mrs x12, far_el1
    mov x13, x30
    adr x16, Lcont
    at s1e1r, x16
    isb
    mrs x17, par_el1
    and x17, x17, #0x0000fffffffff000
    and x16, x16, #0xfff
    orr x17, x17, x16
    msr elr_el1, x17
    mov x16, #0x3c5
    msr spsr_el1, x16
    mrs x16, sctlr_el1
    bic x16, x16, #1
    msr sctlr_el1, x16
    eret
Lcont:
    movz x16, #0x0000
    movk x16, #0x3ef0, lsl #16
    movk x16, #0x1, lsl #32
    mov x15, #4
    str x15, [x16, #8]
    str x10, [x16, #16]
    str x11, [x16, #24]
    str x12, [x16, #32]
    str x13, [x16, #40]
    str x17, [x16, #48]
    movz x15, #0x4444
    movk x15, #0x3333, lsl #16
    movk x15, #0x2222, lsl #32
    movk x15, #0x1111, lsl #48
    str x15, [x16]
    dsb sy
    movz x0, #0x0008
    movk x0, #0x8400, lsl #16
    hvc #0
1:  b 1b
Lpass:
    ldp x16, x17, [sp], #16
    nop
    b .
