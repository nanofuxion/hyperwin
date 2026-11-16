// EL0 cycle-counter emulation for Windows on a VM without a PMU (stock AVF: crosvm --no-pmu).
//
// Windows user-mode code reads PMCCNTR_EL0 directly (the CRT's security cookie and timing
// code do it in every process). Without a PMU that read traps, and a critical process dies
// (stop code 0xC000021A). trace.c copies this stub into spare space in ntoskrnl and routes
// the kernel's "synchronous exception from EL0" entry through it. It handles exactly one
// case, "mrs xN, PMCCNTR_EL0" executed at EL0, by returning the virtual counter in xN and
// stepping over the instruction. Everything else goes to the kernel's own handler untouched.
//
// The trap arrives in one of two shapes:
//   EC 0x18  system-register trap, ISS names PMCCNTR_EL0 (QEMU with pmu=off)
//   EC 0x00  undefined instruction, ISS empty (KVM injects this when the vCPU has no PMU)
// For EC 0 the instruction itself is read from the faulting address with an unprivileged
// load, so PAN does not matter and nothing but that exact instruction is emulated.
//
// The last two words are the kernel's displaced first instruction (sub sp, sp, #0x370) and a
// branch back to its second instruction, which trace.c fills in.
    .text
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
    mrs x15, spsr_el1
    and x15, x15, #0x1f
    cbnz x15, Lpass                 // only AArch64 EL0
    mrs x17, elr_el1
    ldtr w17, [x17]                 // the faulting instruction, read as EL0
    movz w16, #0x9d00
    movk w16, #0xd53b, lsl #16      // mrs x0, PMCCNTR_EL0
    and w15, w17, #0xffffffe0
    cmp w15, w16
    b.ne Lpass
    and x15, x17, #0x1f
Lemulate:
    mrs x17, cntvct_el0
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
    eret
Lpass:
    ldp x15, x16, [sp]
    ldr x17, [sp, #0x10]
    add sp, sp, #0x20
    sub sp, sp, #0x370
    b .
