# Baseline: static 32-bit Linux program (int 0x80), for checking BoxedWine's existing path.
    .globl _start
    .text
_start:
    mov $4, %eax
    mov $1, %ebx
    mov $msg, %ecx
    mov $len, %edx
    int $0x80
    mov $252, %eax
    xor %ebx, %ebx
    int $0x80
    .section .rodata
msg: .ascii "hello from 32-bit guest\n"
    len = . - msg
