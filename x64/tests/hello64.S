# Minimal static x86-64 Linux program: write(1, msg, len); exit_group(0)
# Uses only: REX.W mov imm, RIP-relative lea, syscall.
    .globl _start
    .text
_start:
    mov    $1, %eax            # SYS_write
    mov    $1, %edi            # fd = stdout
    lea    msg(%rip), %rsi     # buf (RIP-relative)
    mov    $len, %edx          # count
    syscall
    mov    $231, %eax          # SYS_exit_group
    xor    %edi, %edi
    syscall
    hlt
    .section .rodata
msg:
    .ascii "hello from x86-64 guest\n"
    len = . - msg
