/* Linux ARM64 Image header. AVF and QEMU both enter here the way SweetCider's
 * mkavfimage stub enters: EL1, MMU off, x0 = device tree. */
.section .text.boot
.global _start
_start:
    b       start
    .long   0
    .quad   0
    .quad   image_end - _start
    .quad   0x2
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241
    .long   0
start:
    adrp    x1, stack_top
    add     x1, x1, :lo12:stack_top
    mov     sp, x1
    bl      hyperwin_main
1:
    wfe
    b       1b

.section .bss
.align  4
.skip   16384
.global stack_top
stack_top:
