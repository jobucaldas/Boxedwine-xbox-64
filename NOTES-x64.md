# BoxedWine x86-64 guest spike — notes

Goal: run a 64-bit (x86-64) guest program through a BoxedWine-based path; long-term target is
Xbox Series Dev Mode (UWP, x64 host, JIT restricted) running 64-bit Windows games via Wine64.

Host used for this spike: Linux 6.12 x86-64, 8 cores, gcc 14.2 (Debian).

## Test programs (`x64/tests/`)

Build: `x64/tests/build.sh` (outputs to `x64/tests/out/`, git-ignored).

| test | what it exercises |
|---|---|
| `hello64.S` | `-nostdlib` static: `mov`, RIP-relative `lea`, `syscall` (write + exit_group) |
| `hello_c.c` | static glibc (non-PIE): TLS via arch_prctl, brk/mmap malloc, printf, 64-bit math, recursion, SSE2 doubles |

## Baseline

(in progress)

## Approach 2 — native passthrough + syscall interception (`x64/native/`)

What: a loader maps a static x86-64 ELF into the current process, builds the Linux initial stack
(argv/envp/auxv), and jumps to the entry point. Guest code runs **natively**. Every guest `syscall`
is trapped with Linux **Syscall User Dispatch** (`prctl(PR_SET_SYSCALL_USER_DISPATCH)`, Linux 5.11+):
syscalls outside a 4 KB trampoline page raise SIGSYS while the selector byte is BLOCK. The SIGSYS
handler swaps FS base (guest TLS <-> host TLS), emulates the syscall, and returns via an
`rt_sigreturn` restorer located inside the allowed trampoline page.

Emulated so far: `arch_prctl(SET_FS/GET_FS)` (guest TLS kept virtual), `brk` (separate guest heap,
not the host's), `set_robust_list` (ignored), `rt_sigaction(SIGSYS)` refused; everything else
forwarded to the host kernel.

Commands:
```
x64/tests/build.sh && x64/native/build.sh
x64/native/out/x64native x64/tests/out/hello64
x64/native/out/x64native x64/tests/out/hello_c x y
x64/native/out/x64native -v x64/tests/out/hello_c     # logs every trapped syscall
```
Output:
```
hello from x86-64 guest

hello from x86-64 glibc guest, argc=3 argv[0]=x64/tests/out/hello_c
64-bit math: 2347ae147ae1478c fib(27)=196418
malloc ok, strlen=100 sizeof(void*)=8
double: 3543.750
```
Result: **works** (standalone, not yet wired into BoxedWine's kernel).

Xbox notes: Syscall User Dispatch is Linux-only. On Windows/Xbox, a Windows x64 PE does not issue
Linux syscalls; it calls into ntdll (Wine's ntdll in the Wine64 case), so the interception point
becomes Wine's own syscall dispatcher (Wine already does exactly this on Linux with SUD). The real
blocker on Xbox for native passthrough is whether the UWP app may map pages executable from
arbitrary bytes (loading a PE image from memory = dynamic code). See recommendation section.

## Approach 1 — extend BoxedWine's CPU to long mode

(in progress)
