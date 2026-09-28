    .section .text
    .globl _start
_start:
    mov x0, #0
1:
    add x0, x0, #1
    cmp x0, #4096
    b.lt 1b
    mov x1, #0
2:
    add x1, x1, #1
    cmp x1, #4096
    b.lt 2b
    b .
