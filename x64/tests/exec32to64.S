# i386 static program that execve()s /bin/hello64: tests a 32-bit -> 64-bit exec switch.
    .globl _start
    .text
_start:
    mov $11, %eax          # execve
    mov $path, %ebx
    mov $argv, %ecx
    mov $envp, %edx
    int $0x80
    mov $1, %eax           # exit(1) if execve failed
    mov $1, %ebx
    int $0x80
    .data
path: .asciz "/bin/hello64"
argv: .long path, 0
envp: .long 0
