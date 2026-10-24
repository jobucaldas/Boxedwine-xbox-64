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

- Deps: everything was preinstalled except `libglu1-mesa-dev` (`sudo apt-get install libglu1-mesa-dev`).
- Build: `make -C project/linux -j8` (release target: x64 JIT, `BOXEDWINE_64 BOXEDWINE_JIT BOXEDWINE_JIT_X64
  BOXEDWINE_MULTI_THREADED BOXEDWINE_MEM_CACHE BOXEDWINE_HOST_EXCEPTIONS`), ~1 min, output
  `project/linux/Build/Release/boxedwine`.
- `boxedwine -root /workspace/x64-root -nozip -novideo /bin/hello32` (static i386 `int 0x80` hello,
  `x64/tests/hello32.S`) prints `hello from 32-bit guest`.
- Same command with the x86-64 `hello64` before any change: hangs (ELF64 rejected, nothing runs).

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

## Approach 1 — extend BoxedWine's CPU to long mode  ✅ WORKS

Design (pragmatic long mode, "64-bit registers, 32-bit address window"):
- **New CPU class `X64CPU`** (`source/emulation/cpu/x64/`): a portable C++ x86-64 interpreter
  (no JIT, so it is valid on hosts where JIT is forbidden). `CPU::allocCPU` returns it when
  `KProcess::isX64` is set, so it plugs into BoxedWine's existing per-thread run loop
  (`platformThread` → `cpu->run()`), signals, termination, etc.
  - Decoder: legacy prefixes, REX (W/R/X/B, SPL..DIL byte regs), ModRM/SIB, RIP-relative,
    FS/GS segment bases, 0F/0F38/0F3A maps.
  - Integer ISA: ALU, shifts/rotates, mul/imul/div/idiv incl. 128-bit (portable helpers, MSVC-safe),
    bit ops, cmov/setcc/jcc, string ops (rep fast paths), cmpxchg/8b/16b, xadd, bswap, cpuid, rdtsc,
    enter/leave, push/pop, lazy flags.
  - SSE/SSE2/SSE3 subset (moves, arithmetic, conversions, packed integer, shuffles, compares).
  - x87: bridged to BoxedWine's existing `common_*` FPU helpers (they take a 32-bit address, which
    works because the guest lives below 4GB).
  - CPUID reports a conservative SSE3-only Ivy Bridge so glibc picks SSE2 string routines.
- **ELF64 loader** (`source/kernel/loader/loader64.cpp`): maps PT_LOAD segments (ET_EXEC in place,
  ET_DYN at `ADDRESS_PROCESS_LOADER`), builds the x86-64 initial stack (argv/envp/auxv with
  AT_RANDOM/AT_PLATFORM/AT_HWCAP/AT_EXECFN), 64-bit signal frames (`rt_sigframe`, `rt_sigreturn`),
  SIGSEGV delivery from the soft-MMU (`KThread::seg_mapper` hook).
- **x86-64 syscall ABI** (`source/kernel/syscall64.cpp`): ~150 syscalls. Ones with identical
  i386 layout are forwarded to the existing i386 handler table (`ksyscallI386`, args staged in
  EBX..EBP); the rest are converted (stat/fstat/newfstatat → x86-64 `struct stat`, iovec, lseek via
  _llseek, mmap with byte offsets, rt_sigaction 64-bit layout, sigaltstack, arch_prctl FS/GS,
  gettimeofday, sysinfo, futex→futex_time64, nanosleep→clock_nanosleep_time64, uname machine=x86_64).
  `BOXEDWINE_X64_STRACE=1` logs every syscall.
- Memory: the guest is constrained to the low 4GB, so the existing 32-bit `KMemory`/soft-MMU, file
  mapping and kernel APIs are reused unchanged. Static non-PIE binaries (0x400000) and everything
  mmap/brk returns fit naturally.

Commands:
```
x64/run-demo.sh                      # builds tests (+boxedwine if missing), runs /bin/hello64
x64/run-demo.sh /bin/hello_c x y     # static glibc program
# equivalent raw command:
project/linux/Build/Release/boxedwine -root x64/out/root -nozip -novideo /bin/hello64
```
Output:
```
Launching "/bin/hello64"
hello from x86-64 guest
Boxedwine shutdown

Launching "/bin/hello_c" "x" "y"
hello from x86-64 glibc guest, argc=3 argv[0]=/bin/hello_c
64-bit math: 2347ae147ae1478c fib(27)=196418
malloc ok, strlen=100 sizeof(void*)=8
double: 3543.750
Boxedwine shutdown
```
(identical to running the binaries natively; hello_c takes ~0.3 s wall time including startup.)

### Approach 1, step 2: dynamic linking, threads, fork/exec (commit 5501b20a)

- **Dynamic linking**: a PIE `gcc -O1` hello linked against the host glibc runs with
  `/lib64/ld-linux-x86-64.so.2` + `libc.so.6` copied into a guest root (ld.so is loaded at
  0xF0000000, libc and the PIE are mmapped from BoxedWine's VFS):
  `boxedwine -root /workspace/x64-dynroot -nozip -novideo /bin/dynhello` → `dynamic x86-64 hello, argc=1`.
- **Threads**: clone/clone3 create a new X64CPU thread (CLONE_SETTLS → guest FS base); LOCK-prefixed
  read-modify-write ops, XCHG, CMPXCHG8B/16B use host atomics on guest RAM (atomic load + CAS,
  instruction retried on contention), since every guest thread is a real host thread.
- **fork/vfork/execve**: forked children keep an X64CPU; `execve` swaps the thread's CPU object
  when the new image has the other architecture (old CPU retired, `platformThreadRun` follows
  `thread->cpu`), so i386 → x86-64 and x86-64 → i386 exec both work.
- Test `x64/tests/mt_threads.c` (4 threads × 100k mutex-protected increments, `__thread` TLS, fork +
  waitpid exit status, fork + execv):
  ```
  boxedwine -root <root> -nozip -novideo /bin/mt_threads /bin/hello64
  threads: counter=400000 tls-sum=604
  fork: child running
  fork: child exit status=42
  hello from x86-64 guest
  exec /bin/hello64: status=0
  ```
  (identical to native; also with `/bin/hello32` as the exec target, and `x64/tests/exec32to64.S`
  — an i386 program that execve()s `/bin/hello64` — prints `hello from x86-64 guest`.)

### Approach 1, step 3: conformance suite + real Debian amd64 userland (commit 3e61bff8)

Conformance (`x64/tests/compare.sh`: each `t_*` test runs natively and under BoxedWine, stdout diffed):
| test | covers | result |
|---|---|---|
| `t_int` | 8–64-bit ALU, div/mod, `__int128`, rol/ror/rcl/rcr/shld/shrd/bsf/bsr/bswap/bt*/adc/sbb/cmpxchg/xadd/lahf/sahf/setcc | PASS |
| `t_str` | mem*/str* on all sizes 0..300 × alignments 0..15, qsort/bsearch, printf/scanf/strtol | PASS |
| `t_misc` | setjmp/longjmp, SIGSEGV/SIGFPE handlers + siglongjmp, raise/sigprocmask/sigpending, file I/O, stat, readdir, env | PASS |
| `t_fp` | SSE float/double, rounding modes, fenv exception flags, libm, strtod, **x87 long double** | FAIL only on long double: BoxedWine's shared x87 core keeps registers as doubles (64-bit mantissa, 80-bit range lost) |

Bugs found and fixed by the suite: late delivery of self-raised signals (i386 frame builder
consumed them), `rt_sigpending` missing, x87 control word 0 (single precision) instead of 0x37F,
MXCSR rounding/flags ignored (now kept in the host fenv), COMISD/CMPPD not signalling invalid on NaN.

Debian trixie amd64 `debootstrap --variant=minbase` root (`/workspace/x64-debroot`, 250 MB):
BoxedWine's VFS doesn't follow host symlinks in a `-root` dir, it uses `<name>.link` files →
`x64/tools/symlinks-to-boxedwine.py <root>` converts them (309 links, merged-/usr).
```
boxedwine -root /workspace/x64-debroot -w / -nozip -novideo /bin/bash -c \
  'echo $((6*7)); ls /usr | head -3; echo pipe:$(echo abc | tr a-z A-Z); python3 -c "import os,threading; print(os.getcwd(), threading.active_count())"'
42
bin
games
include
pipe:ABC
/ 1
boxedwine ... /usr/bin/python3 -c 'import sys; print(sum(range(100)), sys.version)'
4950 3.13.5 (main, Aug 10 2026, 12:06:59) [GCC 14.2.0]          (0.6 s wall)
```
i.e. stock dynamically linked distro binaries (bash, coreutils, python3 3.13) run: ld.so, libc,
fork/exec pipelines, command substitution, locale fallback, threads.
Added for this: sockets (+ sendmsg/recvmsg msghdr/cmsg conversion), select/pselect6/ppoll,
fcntl locks, statfs, epoll_pwait, signalfd4, rt_sigsuspend/sigtimedwait, interrupted-syscall restart.

### Approach 1: performance

`bench.c` (fib(32) + 50M-iteration xorshift loop + 5M double loop, `gcc -O2 -static`):
native 0.10 s, BoxedWine x64 interpreter 12.1 s (~120x). callgrind on a reduced run: ~270 host
instructions per guest instruction; 41% in `step()` (prefix/opcode dispatch), 17% `decodeModrm`,
7% `alu`, 6% `getFlags`. The interpreter re-decodes every instruction; a pre-decoded block cache
would give a few x, but game-level speed needs a JIT or native execution (see recommendation).

## Approach 3 — integrate an existing emulator (Blink) — evaluated, not adopted

Built jart/blink `f006a4fc` (`./configure && make`, 3 s) in `/workspace/x64-approach3`.
- `blink hello64` → `hello from x86-64 guest`. `blink hello_c` → **SIGSEGV** (glibc stdio/exit path
  crashes in this Blink build; raw `write()` + `_exit` works).
- Same `bench.c`: native 0.10 s; Blink JIT 0.77 s (7.7x); Blink `-j` (no JIT) 12.2 s (~122x);
  BoxedWine x64 interpreter 12.1 s. → Our interpreter is on par with Blink's interpreter.
- Integration: Blink's CPU core (~9k lines + 4.4k SSE + 1.3k x87) is cleanly separated only on the
  CPU→syscall edge (`OpSyscall`); its memory model (own linear map / page tables, needs host
  `mmap MAP_FIXED` + SIGSEGV) conflicts with BoxedWine's KMemory, its syscall layer (22k lines)
  duplicates BoxedWine's kernel, and it is POSIX-only (no MSVC build: `__int128`, `__builtin_*`,
  inline asm, sigaction/siglongjmp). Its JIT only targets x86-64/aarch64 hosts and needs RWX/W^X
  code memory, same constraint as any JIT on Xbox.
- Verdict: no advantage over the native BoxedWine X64CPU for an Xbox (MSVC/UWP) build; useful as a
  reference for a future JIT (its "path" JIT design is small: ~6k lines).

## Approach 4 — WoW64 — feasibility analysis (no code)

- Wine's new WoW64 (Wine 9+, `--enable-archs=i386,x86_64`) runs **32-bit PE code inside a 64-bit
  Unix process**; 32-bit code is entered via a 0x23 code segment (x86-64 host) or via a CPU backend
  DLL (`wow64cpu.dll` natively, `xtajit`/FEX/`wowbox64.dll` box64 on ARM64, see Hangover). There is
  no "reverse WoW64": x86-64 instructions cannot execute in a 32-bit context, and a 32-bit
  infrastructure cannot host 64-bit PE code without a 64-bit CPU core underneath.
- What WoW64 *does* offer: the 64-bit Unix side + thunking is exactly how BoxedWine would host 32-bit
  games once the 64-bit guest path exists (run Wine64 with new-WoW64 → both 32-bit and 64-bit PE).
  i.e. WoW64 is a consumer of approach 1, not an alternative to it.
- Wine-on-Windows-host precedent: none found (Hangover targets ARM64 Linux). On Xbox, Wine's Unix
  side has to be provided by an emulated Linux (BoxedWine) — that is the BoxedWine model.

## Native step 1 — 64-bit PE test programs under distro wine64 (Linux host)

Stock Debian Wine (`sudo apt-get install wine64`, wine-10.0 Debian 10.0~repack-6), no BoxedWine
involved: the test programs' x86-64 code runs natively on the host CPU, with Wine's PE-side DLLs
(ntdll/kernel32/msvcrt from `/usr/lib/x86_64-linux-gnu/wine/x86_64-windows`) and Wine's Unix side
providing the OS layer. This is the native-execution baseline to compare BoxedWine against.

Test programs (`x64/pe/tests/`, built with mingw-w64 gcc 14 via `x64/pe/tests/build.sh`):
- `hello_k32.c` — no CRT, kernel32 only (GetStdHandle/WriteFile/ExitProcess).
- `hello.c` — standard mingw CRT: printf, argv, `NtCurrentTeb()` self-pointer check, malloc,
  64-bit math, doubles, 2 threads with InterlockedIncrement, `__thread` TLS.

Commands (from the repo root):
```
sudo apt-get install -y wine64 gcc-mingw-w64-x86-64
x64/pe/tests/build.sh
export WINEPREFIX=$HOME/.wine64-step1 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
/usr/lib/wine/wine64 x64/pe/tests/hello_k32.exe
/usr/lib/wine/wine64 x64/pe/tests/hello.exe a b
```
Real output (stderr dropped; exit codes appended):
```
hello from a native x86-64 PE (kernel32 only)
exit=0
hello.exe: native x86-64 Windows PE, argc=3 argv[0]=Z:\workspace\Boxedwine-xbox-64\x64\pe\tests\hello.exe
TEB self-check: ok, sizeof(void*)=8
malloc: heap, 64-bit math 123456789000, double 6.283
threads: shared=200000 exit codes=6,7 main tls=5
exit=0
```
Notes: the first run creates the prefix (~7.5 s). stderr only contained `XDG_RUNTIME_DIR` warnings
and `failed to open C:\windows\syswow64\rundll32.exe` (no 32-bit Wine installed; harmless here).
A prefix directly under `/tmp` is refused by Wine ("'/tmp' is not owned by you").
