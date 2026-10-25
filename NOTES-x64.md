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

## Native path via Wine loader (step R2)

Goal of R2: run our owned guest PE through **Wine's own, unmodified PE loader** — the
mature, auditable, open-source load path — instead of any hand-rolled in-process mapper.
Two things are shown here: (a) a tiny launcher that invokes Wine's public loader binary,
and (b) proof the guest image is mapped by Wine's loader (not by us), on this modern
PE-split / new-WoW64 Wine.

### This Wine is the modern PE/Unix-split (new WoW64) build
`wine-10.0 (Debian 10.0~repack-6)`. The loader is split into a PE side and a Unix side:
```
/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll  -> PE32+ executable for WINE (DLL), x86-64   (Wine's PE-side loader)
/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so      -> ELF 64-bit LSB shared object, x86-64        (Unix host side)
x86_64-windows also ships: kernel32.dll, wow64.dll, wow64cpu.dll, wow64win.dll
```
ntdll.dll is a genuine PE that performs the Windows-side image loading (map sections,
relocations, import resolution, TEB/PEB, entry call); ntdll.so is the ELF bridge to the
host OS. This is exactly the architecture R2 targets: the guest .exe is *input data* to
ntdll.dll's loader; our code never maps an image.

### (a) Tiny launcher that uses Wine's public invocation mechanism
`x64/pe/launcher/run_via_wine.c` (built native: `cc -O2 -o run_via_wine run_via_wine.c`).
It does one thing: `execv()` Wine's own loader binary (`/usr/lib/wine/wine64`, overridable
via `WINE_LOADER`) with the guest `.exe` as an argument. No mapping / relocation / import
work is done by the launcher — all of that is Wine's. This is the piece that would live
inside the Dev Mode UWP app, calling the Wine loader bundled in the app's own storage.

Command (from repo root):
```
export WINEPREFIX=$HOME/.wine64-step1 WINEDLLOVERRIDES="mscoree,mshtml="
WINEDEBUG=-all x64/pe/launcher/run_via_wine x64/pe/tests/hello_k32.exe
WINEDEBUG=-all x64/pe/launcher/run_via_wine x64/pe/tests/hello.exe a b
```
Real output:
```
[launcher] exec /usr/lib/wine/wine64 -> x64/pe/tests/hello_k32.exe (handing guest to Wine's loader)
hello from a native x86-64 PE (kernel32 only)
exit=0
[launcher] exec /usr/lib/wine/wine64 -> x64/pe/tests/hello.exe (handing guest to Wine's loader)
hello.exe: native x86-64 Windows PE, argc=3 argv[0]=Z:\workspace\Boxedwine-xbox-64\x64\pe\tests\hello.exe
TEB self-check: ok, sizeof(void*)=8
malloc: heap, 64-bit math 123456789000, double 6.283
threads: shared=200000 exit codes=6,7 main tls=5
exit=0
```

### (b) Proof the guest is loaded by Wine's own loader
Run the launcher with `WINEDEBUG=+loaddll` so Wine's loader logs every module it maps:
```
export WINEPREFIX=$HOME/.wine64-step1 WINEDLLOVERRIDES="mscoree,mshtml="
WINEDEBUG=+loaddll x64/pe/launcher/run_via_wine x64/pe/tests/hello_k32.exe
```
Real trace (stderr):
```
[launcher] exec /usr/lib/wine/wine64 -> x64/pe/tests/hello_k32.exe (handing guest to Wine's loader)
0124:trace:loaddll:build_module Loaded L"Z:\\workspace\\Boxedwine-xbox-64\\x64\\pe\\tests\\hello_k32.exe" at 0000000140000000: native
0124:trace:loaddll:build_module Loaded L"C:\\windows\\system32\\kernelbase.dll" at 00006FFFFF3F0000: builtin
0124:trace:loaddll:build_module Loaded L"C:\\windows\\system32\\kernel32.dll" at 00006FFFFFA40000: builtin
hello from a native x86-64 PE (kernel32 only)
exit=0
```
Reading: `build_module` is Wine's PE loader (in ntdll.dll). It maps our guest
`hello_k32.exe` at its preferred base `0x140000000` (`native` = the real guest image),
then resolves its import `kernel32.dll` (+ `kernelbase.dll`) as Wine `builtin` PE DLLs.
The guest's x86-64 code then runs natively on the host CPU. Our launcher contributed no
loader code; the entire map/relocate/import/entry sequence is Wine's.

### How this packages inside a Dev Mode UWP app (design, no code)
- The UWP app's install directory (its own app storage) ships: Wine's loader + the PE-side
  DLLs (ntdll.dll/kernel32.dll/...) + the Unix side, and the guest `.exe` the user owns.
- Xbox Series is x86-64: owned x64 guest code runs natively. Wine's Unix side
  needs a POSIX host implemented with documented UWP capabilities; Linux is only
  the R4H stand-in. No ARM64 translation or JIT is required by this architecture.
- The launcher role (this file) is unchanged on Xbox: hand the owned guest `.exe` to Wine's
  own loader as input data. No image is mapped by our code, nothing is hidden from the OS
  loader, nothing is injected into other processes, and only files from the app's own
  storage are loaded. Guest DLL resolution stays inside the app's bundled Wine prefix.

Skipped sub-part: none. R2 asked specifically for Wine's own loader path (not a hand-rolled
in-process mapper — that was declined earlier, see REFUSED-STEP2), and that is exactly what
is demonstrated above.

> **Correction (R3, applied to the packaging note above):** Xbox is not ARM64.
> Xbox Series X|S have an 8-core **x86-64** AMD Zen 2 CPU. x86-64 guest code can run
> directly on the CPU, so no box64/FEX/`wowbox64` translator is needed. What's missing on
> Xbox is not an ISA translator but a **POSIX host for Wine's Unix side**: there's no Linux
> kernel inside a UWP app. That gap is still the BoxedWine-style Linux-syscall layer
> (Approach 1), or something that replaces Wine's `ntdll.so` Unix side.

## How GameNative boots Steam games (step R3, research)

Sources read: `github.com/utkarshdalal/GameNative` at `dc245441` (2026-10-24, GPL-3.0) and
`github.com/oxters168/Pluvia` at `467a631f` (2026-02-23, GPL-3.0; development has stopped and
the README points to GameNative, which started as a Pluvia fork). File references below are
to GameNative unless noted.

### Overall stack (Android)
- **App layer (Kotlin):** a Steam client written on **JavaSteam** (a Java port of SteamKit2,
  using a GameNative fork `io.github.joshuatam:javasteam`). It handles login (the user's own
  account, with a refresh token stored in prefs), licence/PICS/app-info, cloud saves, friends,
  and downloads through `javasteam-depotdownloader` (a port of SteamRE's DepotDownloader). Games are
  fetched depot-by-depot with the user's licences and decryption keys into
  `<app files>/Steam/steamapps/common/<installdir>`.
- **Runtime layer (Winlator, inherited from Pluvia):** `com.winlator.*`. An "imagefs" Linux
  rootfs (glibc, or a bionic variant), a Wine/Proton build (`proton-9.0`, `proton-10.0`,
  `proton-11.0` in x86_64 and arm64ec variants), and **box64 / FEX / wowbox64** to run
  x86-64 code on ARM phones. There's a **Java X11 server** (`com/winlator/xserver`) that
  Wine's winex11 draws into, PulseAudio/ALSA bridges, and Vulkan drivers (Turnip+Zink for Adreno,
  VirGL, "vortek"). A game is a Winlator *container* (a Wine prefix plus settings).
  The game directory is exposed as a Wine drive (`A:`/`D:`) or as
  `C:\Program Files (x86)\Steam\steamapps\common\<game>`.
- **Choosing which exe to launch:** `SteamService.getInstalledExe()` reads the app's PICS
  `config/launch` entries (Windows targets), checks them against the depot manifests, and
  picks the flagged launch exe (falling back to the largest exe). Users can override it
  per container. The final Wine command line is always `winhandler.exe "<target>"`
  (`XServerScreen.kt` ~L5150-5290). `winhandler` is Winlator's in-prefix helper process.

### How the Steam client / Steamworks API is handled — five launch modes
A Steamworks game calls `steam_api64.dll`, which wants a running, logged-in Steam client
(`steamclient64.dll` loaded by `steam.exe`) for the same account that owns the game.
GameNative provides that in one of these ways (per container, `Container.steamType` /
`launchRealSteam` / `useLegacyDRM`):

1. **Real Valve Steam client (`launchRealSteam`, GUI):** the Windows `steam.exe` runs inside
   Wine: `steam.exe -silent -vgui -tcp -nobigpicture -nofriendsui -nochatui -nointro
   -applaunch <appid>`. `SteamTokenLogin` writes the user's own refresh token into Steam's
   config so the client auto-logs in. Steam then launches the game the normal way. This
   is the same model Proton uses on Linux, except Proton's `lsteamclient.dll` bridges to the
   *native Linux* Steam client instead of a Windows one.
2. **Headless real client ("steamhost", default `steamType=headless`, used when
   `launchRealSteam` is on):** a downloaded archive (`steamhost-20261021.tzst`, "Valve client
   tree build 1788652215 + headless host as steam.exe"). It takes `STEAMHOST_ACCOUNT`,
   `STEAMHOST_TOKEN` (sealed refresh token), `STEAMHOST_APPID`, `STEAMHOST_GAME_CMD`, and so on, logs
   in with Valve's client binaries without the CEF UI, and starts the game. Progress is
   reported through a `steamhost_status` file. The steamhost source isn't in the repo; only
   the binary archive is downloaded.
3. **"Bionic Steam":** Valve's Android-arm64 `libsteamclient.so` runs in the Android process.
   Proton's `lsteamclient.dll` (PE) plus its unixlib bridge the game's Windows `steam_api`
   calls to that native client, the same way Proton does on desktop Linux. The lsteamclient
   unixlib ABI is locked to the Wine version, so they ship one archive per Proton build
   (`BionicSteamAssetsDependency.kt`).
4. **Open reimplementation — gbe_fork "steampipe" (`useLegacyDRM`, or `.bat` targets):** the
   game's own `steam_api(64).dll` is backed up as `.orig` and **replaced** with a build of
   **gbe_fork** (Goldberg Steam Emulator fork, LGPL-3.0; the strings in
   `assets/steampipe/steam_api64.dll` show `D:\a\gbe_fork\...`). `ensureSteamSettings()`
   writes `steam_settings/` (appid, installed depots, DLC list, account name and SteamID,
   language, save path, and **an encrypted app ticket the app requests from Steam for the
   logged-in user**, `SteamService.getEncryptedAppTicket`). The game runs with no Steam
   client at all.
5. **gbe_fork ColdClient (the default when the real client is off):** `replaceSteamclientDll()`
   extracts `experimental-drm-20260116.tzst` (gbe_fork's experimental `steamclient(64).dll` plus
   `steamclient_loader_x64.exe`) into the Steam directory. It restores the game's original
   `steam_api` and writes `ColdClientLoader.ini` (exe, args, working dir, SteamClient DLL
   paths). It launches `steamclient_loader_x64.exe`, which starts the game with the
   emulated steamclient in place of Valve's.

Pluvia (the predecessor) only had a much smaller version of mode 4: a ~200 KB mingw-built
`steam_api(64).dll` stub that forwards a few `SteamAPI_*` calls (Init, IsSteamRunning,
RunCallbacks, ...) over TCP `127.0.0.1:34865` to `SteamPipeServer.java` in the Android app.
It's documented as supporting **DRM-free games only**.

**DRM note.** GameNative also has an optional **Steamless** step (`unpackExecutableFile`,
`Container.unpackFiles`, default `false`). It runs `Steamless.CLI.exe` on the game exe to
strip SteamStub DRM and swaps in `<exe>.unpacked.exe`. That is DRM removal, so it is **out
of scope for this project and must not be reused.** Modes 4/5 (swapping Valve's steam_api /
steamclient for an emulator) are also a legal and ToS grey area even with a real ownership
ticket: they replace the vendor's licence-checking component. For João's runtime, the
clearly legitimate choices are modes 1–3: Valve's own client binaries, logged in with his
own account, on the device.

### Graphics: DXVK / VKD3D(-Proton)
- `extractDXWrapperFiles()` (`XServerScreen.kt` ~L6059): on first boot it clones Wine's
  original `d3d8/9/10/10_1/10core/11/12/12core/dxgi/ddraw` DLLs. It then extracts the chosen
  wrapper into `C:\windows\system32` (+`syswow64`) and sets `WINEDLLOVERRIDES=...=n`.
  Choices are `wined3d` (restores the originals), `dxvk-<ver>` (1.9.2, 1.10.x/1.11.1 "sarek"
  for old drivers, 2.4.1, 2.6.1-gplasync, 2.7.1, ...), `vkd3d-<ver>` (VKD3D-Proton 2.6–3.0b,
  always paired with a DXVK ≥ 2.1 for d3d11/dxgi), and `cnc-ddraw` for old DirectDraw.
  Env knobs: `DXVK_ASYNC`, `DXVK_FRAME_RATE`, `VKD3D_FRAME_RATE`, `DXVK_LOG_LEVEL`,
  `VKD3D_DEBUG`.
- The Vulkan driver is separate (`extractGraphicsDriverFiles`): Turnip (Mesa Freedreno
  Vulkan) for Adreno, with Zink for GL; or VirGL; or the vendor driver through "vortek".
  On Xbox the matching piece would be a Vulkan implementation reachable from inside the
  UWP sandbox. There's no official one, so a Vulkan-on-D3D12 layer (e.g. Mesa "dozen"/dzn)
  is what DXVK/VKD3D-Proton would sit on.

### What a Dev Mode UWP app could reuse
| Piece | Reusable? | Notes |
|---|---|---|
| JavaSteam / SteamKit2 / DepotDownloader logic (login, licences, PICS, depot download, cloud saves) | **Yes, as a design and as libraries** | SteamKit2 (C#, LGPL-2.1) fits a UWP/.NET app directly. Uses only WebSocket CMs and HTTPS CDN (verified below: anonymous steamcmd works through a 443-only proxy). Needs João's login at runtime. |
| Launch-target selection from PICS `config/launch` | Yes | Pure logic. |
| Container model (prefix + DLL overrides + per-game env / fixes, `gamefixes/STEAM_*.kt`) | Yes | Data and logic only. |
| DXVK / VKD3D-Proton install-by-override | **Yes** | Exactly what was verified below on Linux. Licences: zlib / LGPL-2.1. |
| Real Valve client (modes 1–3) | Yes, in principle | Needs João's login. Mode 3's idea (Proton `lsteamclient` bridging to a *native* client) has no Xbox equivalent, since Valve has no Xbox steamclient. So on Xbox it's mode 1/2: Windows `steam.exe`/steamclient inside the same Wine, which needs 32-bit (WoW64) support for the bootstrapper. |
| gbe_fork steampipe / ColdClient (modes 4–5), Steamless | **No** | Replaces or strips licence and DRM components; excluded by project rules. |
| Winlator X server, box64/FEX/wowbox64, Turnip, Android imagefs | **No** | Android/ARM-specific. On Xbox: x86-64 runs natively; display is a CoreWindow/SwapChainPanel; the "Linux" is BoxedWine's syscall layer. |

## Step R3 prototype — Steam content + DXVK/VKD3D under wine64, no account

Everything ran on this Linux box under Debian `wine 10.0~repack-6` (`wine64` plus `wine32:i386`
for WoW64) on Xvfb, with **no Steam account**: steamcmd `+login anonymous` only, plus
open-source DXVK/VKD3D-Proton. Runner: `x64/pe/r3/run_r3.sh`. Probes: `x64/pe/r3/*.c`.

### Network note (this sandbox)
Egress is a proxy that allows **HTTPS on 443 to some hosts only**. Plain HTTP (port 80) and
Steam's 27015-27050 ports are blocked. steamcmd's first-run self-update uses plain HTTP, so
it was bootstrapped by hand: the three zips listed in
`https://media.steampowered.com/client/steam_cmd_linux` were downloaded over HTTPS and
unpacked into the steamcmd dir. After that, steamcmd updated itself and logged in anonymously
over WebSocket CMs on 443.

### 1. steamcmd anonymous download (Windows depots)
```
./steamcmd.sh +@sSteamCmdForcePlatformType windows \
  +force_install_dir /workspace/r3-games/sdkredist  +login anonymous +app_update 1007 validate \
  +force_install_dir /workspace/r3-games/valheim-ds +app_update 896660 validate +quit
...
Connecting anonymously to Steam Public...OK
Waiting for client config...OK
Waiting for user info...OK
Success! App '1007' fully installed.          # Steamworks SDK Redist (Valve steamclient64.dll etc.), 65 MB
Success! App '896660' fully installed.        # Valheim Dedicated Server, 2.0 GB
```

### 2. A real Steamworks x64 game binary runs under wine64: Valheim Dedicated Server
`valheim_server.exe` (PE32+ x86-64, Unity 6000.0.75f1 + Mono) with Valve's own
`steam_api64.dll` / `steamclient64.dll`, headless (`-nographics -batchmode`):
```
Initialize engine version: 6000.0.75f1 (26349cd2a5c8)
Using steam APPID:892970
Steam game server initialized
Load world: R3world (R3world)
Opened Steam server
Loading: Done. Total seconds sinze ZNet start: 31.9 ...   (world generated: 183 locations)
Game server connected            # run 1: anonymous gameserver logon to Steam succeeded after 44 s
Loaded L"Z:\...\valheim_server.exe" at 0000000140000000: native
Loaded L"Z:\...\UnityPlayer.dll" ...: native
Loaded L"Z:\...\Plugins\x86_64\steam_api64.dll" ...: native
Loaded L"Z:\...\steamclient64.dll" ...: native
```
Two later re-runs (`run_r3.sh all`, then `run_r3.sh valheim`, in a fresh WoW64 prefix)
matched everything up to "Opened Steam server", but the logon ended
`Game server connected failed` after about 3 minutes. So the logon succeeded in 1 of 3 runs. The log
also shows `CalcUnIPThisBox - GetAdaptersAddresses returned 13` from Wine's iphlpapi inside
the sandbox. The game server's CM logon through this 443-only proxy is therefore
intermittent here. Both outcomes are recorded in `DONE-R3`.

### 3. Steamworks *client* API without a client (what a normal game needs)
`steamapi_probe.exe` loads Valve's `steam_api64.dll` and calls the public flat API:
```
SteamAPI_GetSteamInstallPath = (none)
SteamAPI_IsSteamRunning      = 0
SteamAPI_Init                = 0 (no logged-in Steam client to talk to)
```
This is the piece that **needs João's login**: a client game's `SteamAPI_Init` only succeeds
against a running Steam client logged into an account that owns the app (GameNative modes
1–3). Not attempted, by design.

### 4. Valve's Windows Steam client under Wine: bootstrap only
`SteamSetup.exe /S` (32-bit NSIS) installs fine once `wine32:i386` is present. Without it:
`failed to load ...\syswow64\ntdll.dll c0000135`. So the Xbox runtime needs a
WoW64-capable Wine for Steam's bootstrapper. Running `Steam.exe` shows the "Updating
Steam…" window. It then fails in this sandbox: the bootstrapper's TLS revocation checks go to
`http://yr1.c.lencr.org` on port 80 (blocked, `STATUS_IO_TIMEOUT`), and the manifest fetch ends with
`Download failed: http error 0 (cdn.steamstatic.com/client/steam_client_win32)` /
`Steam needs to be online to update`. On a normal network the next stop is the login
screen, which needs João's account anyway.

### 5. Graphics: DXVK 3.1.1 and VKD3D-Proton 3.0.1 on lavapipe (software Vulkan)
`d3d11_probe.exe` / `d3d12_probe.exe` create a window plus a flip-model swapchain, clear,
present 30 frames, and report which DLL was used (the Wine-builtin check looks for the
`"Wine builtin DLL"` marker at DOS-header +0x40).
```
# Wine builtins
d3d11.dll kind: Wine builtin (wined3d)    adapter: NVIDIA GeForce GTX 470   readback 0,128,255,255
d3d12.dll kind: Wine builtin (vkd3d)      presented 30 frames, fence=30
# DXVK/VKD3D-Proton copied to system32 + WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,d3d9,d3d8,d3d12,d3d12core=n
info:  DXVK: v3.1.1
info:  Found device: llvmpipe (LLVM 19.1.7, 256 bits)
d3d11.dll kind: native (e.g. DXVK)        adapter: llvmpipe ...  readback pixel(0,0) RGBA = 0,128,255,255
vkd3d-proton - applicationVersion: 3.0.1.
d3d12.dll kind: native (e.g. VKD3D-Proton) adapter: llvmpipe ...  presented 30 frames, fence=30
```
Screenshots: `x64/pe/r3/out/d3d11-dxvk.png` (blue) and `d3d12-vkd3dp.png` (orange).

### What ran / what's missing
- **Ran:** anonymous steamcmd download of Windows depots. A real x64 Steamworks game binary
  (Valheim DS: Unity + Mono + Valve steam_api64/steamclient64) under wine64, including an
  anonymous Steam gameserver logon (1 of 3 runs through this proxy). D3D11 through DXVK and
  D3D12 through VKD3D-Proton on Vulkan, installed the same way GameNative/Proton do it.
  Steam's Windows installer.
- **Needs João's login (not done):** downloading owned client games, `SteamAPI_Init` for
  client games, and logging the Steam client in.
- **Blocked by this sandbox's network:** completing the Steam client bootstrap (port-80
  CRL checks).
- **Not reusable / excluded:** gbe_fork steam_api/steamclient replacement and Steamless.
- **Still open for Xbox:** a POSIX host for Wine's Unix side inside UWP (BoxedWine layer),
  WoW64 for 32-bit bootstrappers, a Vulkan driver inside the UWP sandbox (Vulkan-on-D3D12),
  and window/input/audio glue (CoreWindow instead of X11).

## POSIX host for Wine on Xbox UWP

Wine's Unix-side ntdll.so and wineserver need a host C/OS surface: Linux-style
syscall numbers and layouts, files and prefix paths, descriptor ownership,
AF_UNIX request/reply sockets and SCM_RIGHTS, poll/epoll, threads/TLS/futexes,
signals and exception contexts, clocks, and mmap/mprotect for data and executable
pages. The intended Xbox execution path is in-process, through Wine's own PE
loader, with executable memory authorized by the documented codeGeneration
capability. No Linux kernel or separate wineserver process is assumed.

BoxedWine already supplies analogous guest Linux services through its emulated
kernel and Windows platform adapters (filesystem/CRT, host threads, memory and
exception handling; see platform/windows/platform.cpp and platformThreads.cpp).
Those adapters serve BoxedWine's emulated CPU/process model; they do not directly
provide Wine64's native Unix ABI. x64/uwp-host is a separate, partial C host surface
with explicit Linux layouts, SysV-facing exports and a Windows backend compiled
with WINAPI_FAMILY=2. It is not yet a linked, booting Wine64 runtime. Existing
Windows VirtualAlloc use is not evidence of Xbox UWP executable-page permission.

Module layout (under x64/uwp-host):

- include/uwph/abi.h: Linux x86-64 constants, types and layouts; uwph.h aggregates the public surface.
- src/common.c: initialization, backend synchronization, logging and errno support.
- src/fd.c: descriptor table and reference-counted object ownership.
- src/fs.c: mount/path translation and Windows read-only regular files via CreateFile2.
- src/socket.c: in-process pipes, AF_UNIX listeners and SCM_RIGHTS transport; network backend remains incomplete.
- src/poll.c: readiness polling, select and epoll over host descriptors.
- src/mem.c: anonymous mappings, protection and tracked RW/RX transitions for requested RWX ranges.
- src/thread.c: host threads, pthread subset, caller stacks, futexes and cooperative tgkill delivery.
- src/teb.c: TEB field save/restore, GS-based current TEB read and arch_prctl subset.
- src/signal.c: signal masks/actions, Windows exception translation and context conversion.
- src/syscall.c: supported raw syscall dispatch; variadic argument slots explicitly use int64_t on LLP64.
- src/time.c: clocks and sleeps through Windows APIs; Linux backend stubs.
- src/process.c: identity, environment, limits and randomness; process creation returns ENOSYS.
- src/server.c: starts a supplied wineserver main callback on an in-process host thread.
- src/dl.c: static Unix-library symbol registry, not an ELF dynamic loader.
- src/stdio.c: minimal buffered streams over host descriptors.
- src/probe.c: runtime API availability and RW-to-RX execution probes.
- tests/: ABI assertions, Unix-side stand-in and smoke checks; tools/: import-surface classification/check script.

Build and validate from x64/uwp-host:

```sh
make out/win/libuwph.a out/win/smoke.exe
WINEDEBUG=-all timeout 60 wine out/win/smoke.exe
make out/linux/abi_check out/linux/smoke
out/linux/abi_check
out/linux/smoke
```

R4-A results (2026-10-24): Windows archive and smoke executable build with
-Werror and WINAPI_FAMILY=2, linking windowsapp for VirtualAllocFromApp. The
MinGW NtCurrentTeb array-bounds warning is avoided with an explicit x64 GS:0x30
read; the stack smoke probe records a numeric address while its local is alive.
The syscall dispatcher reads only the supported call's argument count.
Linux ABI assertions pass; smoke reports **54 passed, 0 failed, open fds at exit: 3**.
The installed wine invocation terminates with **Bad system call (exit 159)**
before producing smoke output; Windows pass/fail counts are unavailable. This
is an execution-environment blocker, not a recorded host-backend test failure.

Known gaps: writable filesystem and directory operations, real network sockets,
full mapping semantics and process semantics, actual Wine64 Unix-side integration,
and on-device validation of TLS/TEB, exceptions, caller stacks and executable
pages. Runtime-resolved API presence and the existing desktop VirtualProtect test
fallback do not establish UWP authorization. Xbox must use the documented app
API/capability path; desktop Wine results cannot certify it. Linux smoke exercises
portable logic and ENOSYS stubs rather than the Windows memory/thread/file backend.

R4H O7 (2026-10-24): final evidence ledger is BLOCKED, no DONE-R4H.
Parent all passes, backend 57/0 and ELF counter checks pass; required combined
regression exits 2 because all four Wine traces attempt wineboot. Real ntdll
open/read/close/mmap/munmap counters are positive with zero selected bypass;
this is selected Linux uwph integration, not full UWP acceptance. O6 Windows
uwph smoke hosted by stock Wine is separately 569/0 (saved results displayed).
Separate Linux wineserver remains unrouted; 160 unsupported imports, 33 retained
libc/internal imports, libc syscall() and two inline syscall sites remain gaps.
O7 Linux-box recording is 17.6 seconds; seven videos validated with ffprobe.
O5 was explicitly replayed to recover damaged command-log provenance, retaining
the original. See /workspace/x64-run/RESULT-R4H-O7.md and O7-verify.log.

## Steam connection and UI

R5 O1 implements offline Python contracts under x64/ui only: explicit mock
login/approval/cancel, a fixed three-entry mock library with stable local IDs,
and install states. Live authentication and all downloads return unavailable.
There are no credentials, token input fields, network requests, persistence,
actual installs, UI screens or launches in this order. Mock metadata does not
establish ownership, entitlement or device/API authorization.

Proposed integration (not implemented): a real Steam connector would support
QR approval and refresh-token authentication in a GameNative/JavaSteam-style
flow for João's own account, retrieve the owned library, and download only
entitled depots through a DepotDownloader-style adapter. Future refresh tokens
must use a platform credential store with token redaction in UI/logs/errors;
no secret storage is implemented here. No accounts are created or used.

The planned controller-friendly shell exposes login, library grid, Install and
Launch. Launch must use Wine's own PE loader through our run_via_wine launcher
and the existing x64/uwp-host harness, retaining its verification and current
wineboot-attempt blocker. Later Xbox integration requires the documented,
in-process UWP host path; Linux subprocess behavior is not that proof.
WinUI 3 targets desktop Windows: a UWP version requires a distinct compatible
XAML/project/API adaptation, not a drop-in port. No UWP package, Xbox execution
or device/API authorization is established here.

O1 validation is HOST-ONLY model/documentation testing plus the parent
uwp-host build/surface check. No uwp-host runtime, launcher or stock Wine
execution is exercised; UI routing evidence and Linux-box video are deferred
to their respective later orders. See x64/ui/README.md for the exact checks.

## R4S O1 trace

`x64/uwp-host/harness/surface-trace.tsv` records the unsupported ntdll.so
imports actually called by routed hello, hello_k32 and the existing owned
`/workspace/steam-owned/games/megabonk/Megabonk.exe` invocation. Priority is
aggregate observed entry count (descending, symbol name breaks ties), not an
implementation commitment. Top 20: snprintf, fopen, __isoc23_fscanf, fclose,
pthread_getspecific, pthread_sigmask, feof, stat, write, pthread_mutex_lock,
pthread_mutex_unlock, writev, getenv, asprintf, sigaddset, fcntl, setlocale,
time, getuid, realpath. No imports are implemented by O1.

Reproduce on this Linux host with
`/workspace/x64-run/lanes/R4S/evidence/run.sh`, then
`python3 /workspace/x64-run/lanes/R4S/evidence/aggregate.py`.
The retained trace.py, per-target JSON/logs, prefix copy and server log live in
that evidence directory. The existing shim and our run_via_wine launcher are
built by the harness; Wine identity/hash inventory is in
`x64/uwp-host/harness/out/wine/identity.json`. GDB launches only our owned
launcher and follows its exec into Wine's own loader. It arms breakpoints at
all 159 unsupported PLT entries and both decoded syscall instructions, rebasing
and rearming across Wine's startup reexec. The remaining unsupported function
import, longjmp (GLOB_DAT rather than PLT), has a separate pending function
breakpoint and is not hit. Counts include startup reexec; each
breakpoint records an actual entry, not an LD_DEBUG binding or static reference.
The TSV is the hit set, not a claim that unlisted imports are supported.

Each target hits 64 unsupported import names before the same child gate.
Both direct sites (ntdll-relative 0x3ad92, 0x3b05d) are armed and **not hit**;
the libc `syscall` import is separately hit once per target. GDB catches
fork/vfork/clone/clone3 syscall entry and kills the owned inferior at its first
clone entry, before a child is created. The backtrace shows libc fork called
from ntdll NtCreateUserProcess; Wine's process log names wineboot.exe --init.
This is a trace up to the child gate, not guest completion or Megabonk gameplay.
No Wine source, loader, gate, or syscall bytes are patched on disk.

Routing proof before that gate: debugger reads of the existing shim's counters
show ntdll caller wrapper/backend pairs open=3, read=108, close=6, mmap=44,
munmap=1 for each target, with bypass={0,0,0,0,0}. Reads do not call functions
in the inferior. These selected calls go THROUGH ntdll -> existing shim ->
public uwph -> Linux passthrough backend. The 64 unsupported imports retain
Linux dependencies; they are not THROUGH uwph. An explicit separate, unshimmed
stock Linux wineserver services only a copied initialized fixture. This is
Linux-host routing evidence, not in-process UWP or Xbox-device acceptance.
No stock-Wine-only PE run is substituted for the routed trace. The wineboot
child gate remains intact. Video: /workspace/x64-run/videos/R4S-O1.mp4.

Exact test after routed hello trace:
`test -s x64/uwp-host/harness/surface-trace.tsv && wc -l x64/uwp-host/harness/surface-trace.tsv`
prints `67 x64/uwp-host/harness/surface-trace.tsv` (header, 64 hit imports,
two explicitly unhit direct sites).

## R4S O2 traced set

The O1 top 20 (snprintf … realpath, see the O1 list above) are implemented
behind public uwph entries with per-symbol counters (`uwph/trace.h`) on all
backends and exported by the harness shim; surface.tsv routed 5 -> 25,
unsupported 160 -> 140. Linux passthrough uses raw syscalls for
stat/write/writev/fcntl/time and host libc via RTLD_NEXT for the native-ABI
objects (FILE, pthread keys/mutexes, sigset, locale, env). Win32/UWP uses
existing uwph objects plus CRT vsnprintf/setlocale; its fscanf is a bounded
integer/string scanner. `tests/traced.c` (run three times by `make test`: stub,
Win32 under wine64, passthrough) checks each symbol and its counter.

THROUGH: routed hello and hello_k32 via run_via_wine -> Wine's own loader ->
ntdll.so -> shim -> uwph complete with every traced counter nonzero (e.g.
hello: fopen 223, __isoc23_fscanf 222, pthread_mutex_lock 1855, write 295).
Counts are process-wide uwph entries. The wine-test gate is still BLOCKED by
the pre-existing wineboot attempt (same in stock control). Linux-host evidence
only; no Xbox/UWP device acceptance. Video: /workspace/x64-run/videos/R4S-O2.mp4.

### R4S O2 reviewer fixes

Win32 integer scanning now stores Linux x64 `l` conversions with explicit
64-bit width, including `%ln`. The regression initializes all destination high
bits and scans hex, signed, and unsigned values above the 32-bit range.
Linux diagnostic FILE bridges now translate private descriptors: fdopen retains
the slot, fopen registers one, fileno returns it, and fclose releases it without
a second native close. Failed fdopen leaves the descriptor owned by its caller.
Borrowed native standard streams are not adopted into private descriptor slots.
Composition regressions cover fdopen/fileno, fopen/fileno/read, fclose invalidation,
and slot reuse. Exact test passes (stub traced 60, Win32 traced 63, passthrough
traced 70; smoke 55/566). Backend/shim checks pass. Fresh launcher runs retain all
20 counters, while the unchanged wineboot gate still blocks both controls and
routed runs. These are Linux harness and Win32-under-Wine results, not Xbox
acceptance; process-wide counters prove entry/routing, not per-ntdll completion.

## R4S O3 fake proc

`make -C x64/uwp-host/harness noproc-test` tests the bounded in-process
provider with host procfs hidden, then invokes the readiness wrapper's exact
`--noproc INSTALL -- COMMAND` interface for hello through our launcher, Wine's
own loader and the uwph shim. The wrapper now accepts `--noproc` and uses an
empty tmpfs for `/proc` for either noproc spelling. Its updated copy is retained
as `harness/uwp_run-noproc.sh`; set `UWPH_UWP_RUN` to use that copy. The installed
`/workspace/xbox-readiness/tests/uwp_run.sh` was updated locally as well.

Snapshots cover self/maps, self/cmdline, cpuinfo; self/exe uses process executable
identity for readlink, realpath and open. No snapshot reads host `/proc`.
Linux maps enumerate ELF load segments via dl_iterate_phdr (anonymous/PE ranges
not yet included); cmdline contains only the executable with a trailing NUL;
cpuinfo advertises the x64 baseline, not console topology. Linux snapshots use
sealed memfds and the existing private/native fd adapters and native FILE ABI.
Win32 snapshots use in-process read-only file objects and VirtualQuery for maps;
this path is build-checked with WINAPI_FAMILY_APP, not console-tested. A separate
prestarted stock wineserver remains a concession. This is a bounded hello PASS,
not full Linux proc fidelity or full UWP acceptance. Video:
`/workspace/x64-run/videos/R4S-O3.mp4`.

O3 review fixes: provider handling is independent of negative errno, including
unsupported host-existing `/proc/version` for both open and fopen. Linux captures
an absolute AT_EXECFN identity at initialization, before chdir; the hidden-proc
provider test launches relatively and checks readlink, realpath and ELF open after
chdir("/"). The acceptance script deletes the previous report and requires a new
process report with positive ntdll open/read/close/mmap/munmap routing, zero bypass,
recursion, unattributed calls and module overflow before printing THROUGH PASS.
## R4X O1 RWX sites

Source inventory only, Wine version 10.0 (`/tmp/wine-src/VERSION`). The initial
source directory had only ntdll under dlls; expanded the available
`/tmp/R4W-wine.orig.tar.xz` (wine-10.0) into it before the full search.
All locations below are relative to `/tmp/wine-src`; line numbers refer to this
source, not the host's installed binary. No loader, host, verifier or policy changes.

### Loader and host protection requests

| Site | Request and condition |
| --- | --- |
| dlls/ntdll/unix/virtual.c:3019 (map_image_view; map_view calls at 3045, 3067, 3073) | Initial image view is READ + EXEC + WRITECOPY, hence writable executable Unix protection. |
| dlls/ntdll/unix/virtual.c:2445 (map_pe_header) | Explicit `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)` for a non-removable image header; later protections narrow it. The supplied regex misses this operand order. |
| dlls/ntdll/unix/virtual.c:2796 (map_image_into_view) | Flat/non-page-aligned image gets READ + WRITECOPY + EXEC across the image. |
| dlls/ntdll/unix/virtual.c:2948–2951 | Final section protections combine WRITECOPY and EXEC if section characteristics request both; conditional on the PE. |
| dlls/ntdll/unix/virtual.c:3539,3552–3555 | Builtin view initially records executable/writable-copy metadata; section flags may combine WRITE and EXEC. `create_view`/`set_page_vprot` here record metadata, not themselves an unconditional RWX mmap. |
| dlls/ntdll/unix/virtual.c:1140–1142,1683–1690 | Conversion plumbing: WRITE/WRITECOPY becomes Unix WRITE, EXEC becomes Unix EXEC; PAGE_EXECUTE_READWRITE and PAGE_EXECUTE_WRITECOPY can both yield RWX. |
| dlls/ntdll/unix/virtual.c:1636,1713,2111–2117 | Legacy `force_exec_prot` adds EXEC to readable mappings/protections, yielding RWX when writable. Conditional compatibility path, not assumed enabled for x64 hello. |
| dlls/ntdll/loader.c:455 (allocate_stub) | RWX allocation for generated missing-import stubs; only when an unresolved import needs one. |
| dlls/ntdll/relay.c:1064,1221 | RWX snoop entry and return thunk allocations; i386 snooping path, not ordinary x64 hello. |
| dlls/ntdll/heap.c:566 | Executable-heap protection selection returns PAGE_EXECUTE_READWRITE when HEAP_CREATE_ENABLE_EXECUTE is set. Normal TLS heap allocation does not set this flag. |

Relocations: `dlls/ntdll/loader.c:2163–2164` requests PAGE_READWRITE,
then `2191–2192` restores `protect_old[i]`; restoration can reinstate RWX only
if the original section already had that protection. The image mapper also
performs relocations while its initial writable image mapping exists.
IAT thunks: `loader.c:1140–1141` similarly uses PAGE_READWRITE and
`1212` restores the old protection; do not mistake IAT data for executable stubs.
TLS: `loader.c:1351,1574–1575,1586` allocates ordinary process-heap storage;
`1363` writes the TLS index and `1609` begins callback handling. There is no
standalone TLS RWX allocation here; callback code inherits image protections.
Syscall dispatcher: `dlls/ntdll/unix/signal_x86_64.c:2495` maps the pointer
page at 0x7ffe1000 READ + WRITE only; `2496` stores the dispatcher pointer.
The dispatcher itself is compiled assembly at `signal_x86_64.c:2712`.
`dlls/ntdll/unix/loader.c:1584` assigns its exported pointer. None of these
three operations independently requests RWX.

### Hello artifact and applicable sites (review correction)

The actual harness target is `x64/pe/tests/hello.exe`: the loop in
`x64/uwp-host/harness/run-wine.sh:59–69` passes that path to
`out/run_via_wine` (alongside the separate hello_k32 target). It is built by
`x64/pe/tests/build.sh:6` from `hello.c`. The inspected existing binary has SHA256
`85e2309af898c58117925c070705715f01b8186bbf8f9b8129fd5ab8161eb8d9`.
It was inspected without rebuilding or executing it.

`file x64/pe/tests/hello.exe` reports PE32+ x86-64, 18 sections.
`objdump -h x64/pe/tests/hello.exe` shows only `.text` as CODE, READONLY;
`.data`, `.bss`, and `.tls` are writable data. A direct little-endian PE-header
inspection (e_lfanew at 0x3c, COFF section count at PE+6, optional-header size
at PE+20, 40-byte section records with Characteristics at record+36) gives:

```text
Machine=0x8664 sections=18 SectionAlignment=0x1000 FileAlignment=0x200
.text    characteristics=0x60000020 W=0 X=1
.data    characteristics=0xc0000040 W=1 X=0
.rdata   characteristics=0x40000040 W=0 X=0
.pdata   characteristics=0x40000040 W=0 X=0
.xdata   characteristics=0x40000040 W=0 X=0
.bss     characteristics=0xc0000080 W=1 X=0
.idata   characteristics=0x40000040 W=0 X=0
.tls     characteristics=0xc0000040 W=1 X=0
.reloc   characteristics=0x42000040 W=0 X=0
remaining nine debug sections: characteristics=0x42000040 W=0 X=0 each
W+X sections=0/18
```

Thus **only two distinct RWX request sites are established for mapping this
hello image**, on the successful ordinary file-backed mapping path:

1. `dlls/ntdll/unix/virtual.c:3019`: initial RWX image-view protection,
   passed to `map_view` at 3045 (or address-selection alternatives 3067/3073).
   `virtual_map_image:3112` calls this before mapping the image contents.
2. `dlls/ntdll/unix/virtual.c:2445`: RWX header mmap.
   `virtual_map_image:3115` calls `map_image_into_view`, whose line 2759 calls
   `map_pe_header`; line 2443 guards this mmap with `!removable`.
   This establishes applicability for the ordinary mmap-capable, non-removable
   local-file path, not a guarantee for fallback/read-only filesystems or an
   execution that fails before image loading.

There is no third RWX request from the final section-protection site
`virtual.c:2948–2951` for this hello: no section has both
IMAGE_SCN_MEM_WRITE (0x80000000) and IMAGE_SCN_MEM_EXECUTE (0x20000000).
The flat-image RWX site at 2796 is also excluded: SectionAlignment is 0x1000,
so `server/mapping.c:892–893` does not set ImageMappedFlat for a 4 KiB page.
Relocations, IAT, TLS and the x64 dispatcher paths are described above and
supply no unconditional third site. Multiple invocations for dependencies
would not make a third distinct source site. This finding is restricted to
the image-mapping path; it does not assert that every dependency or later CRT
behavior has only two RWX sites. A third startup-wide hit cannot be supplied
from this static evidence and is not invented.

These are statically applicable requests, not dynamically observed hits.
THROUGH-uwph: not run; launcher: inspected only; stock Wine: not run;
classification: source/artifact inspection only (neither build-only validation
nor runtime/Xbox acceptance). No video was generated for this textual search.

### Exact test and complete search output

Command: `rg -n 'PROT_EXEC.*PROT_WRITE|PAGE_EXECUTE_READWRITE|0x40.*map' /tmp/wine-src`

Exit 0; 235 matching lines. Complete output retained below so test-only uses,
constant definitions, protection checks, legacy 16-bit callers and unrelated
0x40 matches are visible without presenting them as x64 loader allocations.
Other production callers include kernelbase/memory.c:300 (executable mapping)
and :637 (temporary write protection), plus the legacy *16 and vmm modules
shown below. This is a lexical inventory, supplemented above for reversed
PROT operand order and indirect image protections.

```text
server/file.h:303:#define FILE_MAPPING_WRITE  0x40000000  /* set for writable shared mappings */
include/winnt.h:805:#define PAGE_EXECUTE_READWRITE          0x00000040
programs/winedbg/info.c:900:            if (mbi.AllocationProtect & (PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY))
programs/winedbg/info.c:902:            if (mbi.AllocationProtect & (PAGE_READWRITE|PAGE_EXECUTE_READWRITE))
programs/winedbg/info.c:906:            if (mbi.AllocationProtect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
programs/winedbg/gdbproxy.c:1613:            if (mbi.AllocationProtect & (PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY))
programs/winedbg/gdbproxy.c:1615:            if (mbi.AllocationProtect & (PAGE_READWRITE|PAGE_EXECUTE_READWRITE))
programs/winedbg/gdbproxy.c:1619:            if (mbi.AllocationProtect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
dlls/winex11.drv/palette.c:55:#define PC_SYS_RESERVED        0x40 /* system palentry is not to be mapped to */
dlls/gdi.exe16/gdi.c:107:                                        MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ole32/tests/propvariant.c:481:                                  MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/user.exe16/message.c:365:                                          MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/mmsystem.dll16/mmio16.c:180:                                   PAGE_EXECUTE_READWRITE);
dlls/mmsystem.dll16/message16.c:866:                                        MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/mmsystem.dll16/mci16.c:563:                                   PAGE_EXECUTE_READWRITE);
dlls/msvcp140_1/tests/msvcp140_1.c:63:            MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ucrtbase/tests/misc.c:1613:    struct rewind_thunk *thunk = VirtualAlloc(NULL, sizeof(*thunk), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/loader.c:1729:        case PAGE_EXECUTE_READWRITE:
dlls/kernel32/tests/loader.c:1765:        { PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY }, /* 0x40 */
dlls/kernel32/tests/loader.c:1766:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, 0 }, /* 0x50 */
dlls/kernel32/tests/loader.c:1767:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, 0 }, /* 0x60 */
dlls/kernel32/tests/loader.c:1768:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, 0 }, /* 0x70 */
dlls/kernel32/tests/loader.c:1773:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE, 0 }, /* 0xc0 */
dlls/kernel32/tests/loader.c:1774:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, 0 }, /* 0xd0 */
dlls/kernel32/tests/loader.c:1775:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, 0 }, /* 0xe0 */
dlls/kernel32/tests/loader.c:1776:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, 0 } /* 0xf0 */
dlls/kernel32/tests/loader.c:1879:        { IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE },
dlls/kernel32/tests/loader.c:1880:        { IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE },
dlls/kernel32/tests/loader.c:1888:        { IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE },
dlls/kernel32/tests/loader.c:1889:        { IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE },
dlls/kernel32/tests/loader.c:1897:        { IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE },
dlls/kernel32/tests/loader.c:1898:        { IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE_WRITECOPY, PAGE_EXECUTE_READWRITE }
dlls/kernel32/tests/file.c:2518:    if ((map_access == PAGE_READWRITE || map_access == PAGE_EXECUTE_READWRITE) &&
dlls/kernel32/tests/debugger.c:1693:    mem = VirtualAllocEx(pi.hProcess, NULL, sizeof(buf), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/sync.c:87:    struct fastcall_thunk *thunk = VirtualAlloc(NULL, sizeof(*thunk), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:91:    addr1 = VirtualAllocEx(GetCurrentProcess(), NULL, alloc_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:95:    addr2 = VirtualAllocEx(NULL, NULL, alloc_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:105:                           PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:233:    b = VirtualProtectEx( hProcess, addr1, 0x2000, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:546:    addr1 = VirtualAlloc(0, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:550:    addr2 = VirtualAlloc(addr1, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:555:    addr2 = VirtualAlloc(addr1, 0x1000, MEM_RESERVE | MEM_COMMIT | AT_ROUND_TO_PAGE, PAGE_EXECUTE_READWRITE);
dlls/kernel32/tests/virtual.c:2303:    call_on_stack = VirtualAlloc( 0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/kernel32/tests/virtual.c:2615:        success = VirtualProtect( (void *)rec->ExceptionInformation[1], 16, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:2645:    success = VirtualProtect( (void *)rec->ExceptionInformation[1], 16, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:2768:        success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:2794:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:2862:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:3000:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:3036:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:3121:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:3186:    success = VirtualProtect( base, size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/kernel32/tests/virtual.c:3233:        { PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READWRITE }, /* 0x40 */
dlls/kernel32/tests/virtual.c:3234:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, 0 }, /* 0x50 */
dlls/kernel32/tests/virtual.c:3235:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, 0 }, /* 0x60 */
dlls/kernel32/tests/virtual.c:3236:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, 0 }, /* 0x70 */
dlls/kernel32/tests/virtual.c:3241:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE, 0 }, /* 0xc0 */
dlls/kernel32/tests/virtual.c:3242:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, 0 }, /* 0xd0 */
dlls/kernel32/tests/virtual.c:3243:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, 0 }, /* 0xe0 */
dlls/kernel32/tests/virtual.c:3244:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, 0 } /* 0xf0 */
dlls/kernel32/tests/virtual.c:3389:        case PAGE_EXECUTE_READWRITE:
dlls/kernel32/tests/virtual.c:3426:        { PAGE_EXECUTE_READWRITE, TRUE }, /* 0x40 */
dlls/kernel32/tests/virtual.c:3427:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, FALSE }, /* 0x50 */
dlls/kernel32/tests/virtual.c:3428:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, FALSE }, /* 0x60 */
dlls/kernel32/tests/virtual.c:3429:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, FALSE }, /* 0x70 */
dlls/kernel32/tests/virtual.c:3434:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE, FALSE }, /* 0xc0 */
dlls/kernel32/tests/virtual.c:3435:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, FALSE }, /* 0xd0 */
dlls/kernel32/tests/virtual.c:3436:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, FALSE }, /* 0xe0 */
dlls/kernel32/tests/virtual.c:3437:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, FALSE } /* 0xf0 */
dlls/kernel32/tests/virtual.c:3516:        { PAGE_EXECUTE_READWRITE, TRUE, PAGE_EXECUTE_READWRITE }, /* 0x40 */
dlls/kernel32/tests/virtual.c:3517:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, FALSE, PAGE_NOACCESS }, /* 0x50 */
dlls/kernel32/tests/virtual.c:3518:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, FALSE, PAGE_NOACCESS }, /* 0x60 */
dlls/kernel32/tests/virtual.c:3519:        { PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, FALSE, PAGE_NOACCESS }, /* 0x70 */
dlls/kernel32/tests/virtual.c:3520:        { PAGE_EXECUTE_WRITECOPY, TRUE, PAGE_EXECUTE_READWRITE }, /* 0x80 */
dlls/kernel32/tests/virtual.c:3524:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE, FALSE, PAGE_NOACCESS }, /* 0xc0 */
dlls/kernel32/tests/virtual.c:3525:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE, FALSE, PAGE_NOACCESS }, /* 0xd0 */
dlls/kernel32/tests/virtual.c:3526:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ, FALSE, PAGE_NOACCESS }, /* 0xe0 */
dlls/kernel32/tests/virtual.c:3527:        { PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE, FALSE, PAGE_NOACCESS } /* 0xf0 */
dlls/kernel32/tests/virtual.c:3557:                if (td[i].prot == PAGE_EXECUTE_READ || td[i].prot == PAGE_EXECUTE_READWRITE)
dlls/kernel32/tests/virtual.c:3612:    if (page_exec_supported) alloc_prot = PAGE_EXECUTE_READWRITE;
dlls/kernel32/tests/virtual.c:3655:                if (td[i].prot == PAGE_EXECUTE_READ || td[i].prot == PAGE_EXECUTE_READWRITE)
dlls/kernel32/tests/virtual.c:3735:    case PAGE_EXECUTE_READWRITE:
dlls/kernel32/tests/virtual.c:3758:    case PAGE_EXECUTE_READWRITE:
dlls/kernel32/tests/virtual.c:3777:    case PAGE_EXECUTE_READWRITE: return PAGE_EXECUTE_WRITECOPY;
dlls/kernel32/tests/virtual.c:3787:    case PAGE_EXECUTE_WRITECOPY: return PAGE_EXECUTE_READWRITE;
dlls/kernel32/tests/virtual.c:3798:    if (access & FILE_MAP_WRITE) return exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
dlls/kernel32/tests/virtual.c:3838:        PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY
dlls/kernel32/tests/virtual.c:3863:        { FILE_MAP_EXECUTE | FILE_MAP_WRITE, PAGE_EXECUTE_READWRITE }, /* 0x22 */
dlls/kernel32/tests/virtual.c:3864:        { FILE_MAP_EXECUTE | FILE_MAP_WRITE | FILE_MAP_COPY, PAGE_EXECUTE_READWRITE }, /* 0x23 */
dlls/kernel32/tests/virtual.c:3867:        { FILE_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE, PAGE_EXECUTE_READWRITE }, /* 0x26 */
dlls/kernel32/tests/virtual.c:3868:        { FILE_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_COPY, PAGE_EXECUTE_READWRITE }, /* 0x27 */
dlls/kernel32/tests/virtual.c:3871:        { FILE_MAP_EXECUTE | SECTION_MAP_EXECUTE | FILE_MAP_WRITE, PAGE_EXECUTE_READWRITE }, /* 0x2a */
dlls/kernel32/tests/virtual.c:3872:        { FILE_MAP_EXECUTE | SECTION_MAP_EXECUTE | FILE_MAP_WRITE | FILE_MAP_COPY, PAGE_EXECUTE_READWRITE }, /* 0x2b */
dlls/kernel32/tests/virtual.c:3875:        { FILE_MAP_EXECUTE | SECTION_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE, PAGE_EXECUTE_READWRITE }, /* 0x2e */
dlls/kernel32/tests/virtual.c:3876:        { FILE_MAP_EXECUTE | SECTION_MAP_EXECUTE | FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_COPY, PAGE_EXECUTE_READWRITE } /* 0x2f */
dlls/kernel32/tests/virtual.c:3892:                    || page_prot[i] == PAGE_EXECUTE_READWRITE || page_prot[i] == PAGE_EXECUTE_WRITECOPY))
dlls/kernel32/tests/virtual.c:3937:                (page_prot[i] == PAGE_READWRITE || page_prot[i] == PAGE_EXECUTE_READWRITE))
dlls/kernel32/tests/virtual.c:3941:            if (page_prot[i] == PAGE_EXECUTE_READ || page_prot[i] == PAGE_EXECUTE_READWRITE)
dlls/kernel32/tests/virtual.c:4014:                   broken(view[j].prot == PAGE_EXECUTE_READWRITE && info.Protect == PAGE_READWRITE) || /* win2k */
dlls/kernel32/tests/virtual.c:4439:                               PAGE_EXECUTE_READWRITE);
dlls/msvcirt/tests/msvcirt.c:520:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/psapi/tests/psapi_main.c:884:    ret = GetMappedFileNameA(GetCurrentProcess(), base + 0x4000, map_name, sizeof(map_name));
dlls/advapi32/tests/security.c:5924:        { PAGE_EXECUTE_READWRITE, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE },
dlls/advapi32/tests/security.c:5961:    mapping = CreateFileMappingW(file, NULL, PAGE_EXECUTE_READWRITE, 0, 4096, NULL);
dlls/msvcrt/tests/cpp.c:131:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/relay.c:1064:                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
dlls/ntdll/relay.c:1221:                                        PAGE_EXECUTE_READWRITE);
dlls/ntdll/heap.c:566:    return (flags & HEAP_CREATE_ENABLE_EXECUTE) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
dlls/ntdll/loader.c:455:                                     MEM_COMMIT, PAGE_EXECUTE_READWRITE ) != STATUS_SUCCESS)
dlls/ntdll/tests/info.c:2979:    ok (mbi.AllocationProtect == PAGE_READWRITE || mbi.AllocationProtect == PAGE_EXECUTE_READWRITE,
dlls/ntdll/tests/info.c:2982:    ok (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE,
dlls/ntdll/tests/large_int.c:483:    code_mem = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:110:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:117:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:124:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:172:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:186:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:222:                                     MEM_RESERVE | MEM_COMMIT | AT_ROUND_TO_PAGE, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:235:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:315:                                        PAGE_EXECUTE_READWRITE, NULL, 0);
dlls/ntdll/tests/virtual.c:324:                                        PAGE_EXECUTE_READWRITE, NULL, 1);
dlls/ntdll/tests/virtual.c:330:    status = pNtAllocateVirtualMemoryEx(NULL, &addr1, &size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, NULL, 0);
dlls/ntdll/tests/virtual.c:630:                                         PAGE_EXECUTE_READWRITE, ext, 1 );
dlls/ntdll/tests/virtual.c:634:                                         PAGE_EXECUTE_READWRITE, ext, 2 );
dlls/ntdll/tests/virtual.c:643:                                         PAGE_EXECUTE_READWRITE, ext, 1 );
dlls/ntdll/tests/virtual.c:660:                                                 PAGE_EXECUTE_READWRITE, NULL, 0 );
dlls/ntdll/tests/virtual.c:665:                                                 PAGE_EXECUTE_READWRITE, ext, 1 );
dlls/ntdll/tests/virtual.c:674:                                                 PAGE_EXECUTE_READWRITE, NULL, 0 );
dlls/ntdll/tests/virtual.c:688:                                                 PAGE_EXECUTE_READWRITE, ext, 1 );
dlls/ntdll/tests/virtual.c:728:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:736:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:745:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:755:                                        PAGE_EXECUTE_READWRITE, ext, 2);
dlls/ntdll/tests/virtual.c:765:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:772:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:779:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:786:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:793:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:803:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:809:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:820:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:824:                                        PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:829:                                        PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/virtual.c:845:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:860:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:873:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:881:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:888:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:895:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:899:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:907:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:915:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:922:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:929:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:937:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:945:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:957:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:969:                                        PAGE_EXECUTE_READWRITE, ext, 1);
dlls/ntdll/tests/virtual.c:2102:    ptr = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/tests/virtual.c:2163:        exec_mem = VirtualAlloc( NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/tests/virtual.c:2796:    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/ntdll/tests/virtual.c:2809:    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/ntdll/tests/virtual.c:2838:    if (0) VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/ntdll/tests/virtual.c:2883:    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/ntdll/tests/unwind.c:2638:                                    PAGE_EXECUTE_READWRITE, &param, 1 ))
dlls/ntdll/tests/unwind.c:3343:                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, &param, 1 ))
dlls/ntdll/tests/unwind.c:3419:    code_mem = VirtualAlloc( NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/unix/sync.c:2197:    case PAGE_EXECUTE_READWRITE:
dlls/ntdll/tests/wow64.c:183:        code_mem = VirtualAlloc( NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/tests/wow64.c:650:    status = NtAllocateVirtualMemory( process, &addr, 0, &size, MEM_RESERVE, PAGE_EXECUTE_READWRITE );
dlls/ntdll/tests/wow64.c:656:                                         MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0, 0xcccccccc );
dlls/ntdll/tests/wow64.c:658:                                         MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0, 0xcccccccc );
dlls/ntdll/tests/wow64.c:677:    status = NtProtectVirtualMemory( process, &addr2, &size, PAGE_EXECUTE_READWRITE, &old_prot );
dlls/ntdll/tests/wow64.c:683:                                         PAGE_EXECUTE_READWRITE, 0, 0xcccccccc, 0xcccccccc );
dlls/ntdll/tests/wow64.c:685:                                         PAGE_EXECUTE_READWRITE, 0, 0xcccccccc, 0xcccccccc );
dlls/ntdll/unix/virtual.c:140:    PAGE_EXECUTE_READWRITE,     /* WRITE | EXEC */
dlls/ntdll/unix/virtual.c:141:    PAGE_EXECUTE_READWRITE,     /* READ | WRITE | EXEC */
dlls/ntdll/unix/virtual.c:231:    return (vprot & VPROT_EXEC) && (vprot & (VPROT_WRITE | VPROT_WRITECOPY));
dlls/ntdll/unix/virtual.c:1683:    case PAGE_EXECUTE_READWRITE:
dlls/ntdll/unix/virtual.c:1685:            *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITECOPY;
dlls/ntdll/unix/virtual.c:1687:            *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITE;
dlls/ntdll/unix/virtual.c:1690:        *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITECOPY;
dlls/ntdll/unix/virtual.c:3019:    unsigned int vprot = SEC_IMAGE | SEC_FILE | VPROT_COMMITTED | VPROT_READ | VPROT_EXEC | VPROT_WRITECOPY;
dlls/ntdll/unix/virtual.c:3185:    case PAGE_EXECUTE_READWRITE:
dlls/msvideo.dll16/msvideo16.c:746:                                      PAGE_EXECUTE_READWRITE);
dlls/ddeml.dll16/ddeml.c:186:                                      PAGE_EXECUTE_READWRITE);
dlls/ntdll/tests/exception.c:1601:    VirtualProtect(code_mem, 1, PAGE_EXECUTE_READWRITE, &old_prot);
dlls/ntdll/tests/exception.c:2024:    ret = VirtualProtect(hook_trampoline, ARRAY_SIZE(hook_trampoline), PAGE_EXECUTE_READWRITE, &old_protect1);
dlls/ntdll/tests/exception.c:2028:            PAGE_EXECUTE_READWRITE, &old_protect2);
dlls/ntdll/tests/exception.c:2163:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:2231:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:3312:    VirtualProtect(code_mem, 1, PAGE_EXECUTE_READWRITE, &old_prot);
dlls/ntdll/tests/exception.c:4872:            PAGE_EXECUTE_READWRITE, &old_protect);
dlls/ntdll/tests/exception.c:5061:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:5140:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:6223:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:6313:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:6382:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:7563:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:7646:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:7711:                          PAGE_EXECUTE_READWRITE, &old_protect );
dlls/ntdll/tests/exception.c:11405:    target_ptr = VirtualAlloc( NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/ntdll/tests/exception.c:11810:    code_mem = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/msvcp60/tests/ios.c:514:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp60/tests/string.c:154:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp100/tests/misc.c:82:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp100/tests/string.c:59:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/concrt140/tests/concrt140.c:40:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp120/tests/msvcp120.c:130:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcr100/tests/msvcr100.c:92:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/vmm.vxd/vmm.c:159:        ret = VirtualAlloc ( address, npages * page_size, MEM_RESERVE, PAGE_EXECUTE_READWRITE );
dlls/vmm.vxd/vmm.c:184:            virt_perm = PAGE_EXECUTE_READWRITE;
dlls/vmm.vxd/vmm.c:241:        case PAGE_EXECUTE_READWRITE:
dlls/vmm.vxd/vmm.c:258:            virt_new_perm |= PAGE_EXECUTE_READWRITE;
dlls/msvcp140/tests/msvcp140.c:96:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/riched20/tests/txtsrv.c:569:                                  MEM_COMMIT, PAGE_EXECUTE_READWRITE);
dlls/commdlg.dll16/filedlg.c:518:    if (!hooks && !(hooks = VirtualAlloc( NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE )))
dlls/krnl386.exe16/dosmem.c:684:        if (DOSMEM_dosmem || !VirtualProtect( NULL, DOSMEM_SIZE, PAGE_EXECUTE_READWRITE, &old_prot ))
dlls/krnl386.exe16/vxd.c:1477:            case PAGE_EXECUTE_READWRITE:  access = FILE_MAP_WRITE; break;
dlls/krnl386.exe16/int31.c:56:                                MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE );
dlls/krnl386.exe16/int31.c:86:                            MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE );
dlls/kernelbase/memory.c:300:        protect = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
dlls/kernelbase/memory.c:451:    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE
dlls/kernelbase/memory.c:623:    case PAGE_EXECUTE_READWRITE:
dlls/kernelbase/memory.c:637:        prot |= (info.Type == MEM_PRIVATE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY;
dlls/kernelbase/sync.c:1022:    case PAGE_EXECUTE_READWRITE:
dlls/kernelbase/tests/process.c:247:    addr = pVirtualAlloc2(NULL, NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE, NULL, 0);
dlls/kernelbase/tests/process.c:423:        PAGE_EXECUTE_READWRITE,
dlls/kernelbase/tests/process.c:457:        PAGE_EXECUTE_READWRITE,
dlls/kernelbase/tests/process.c:531:    file = pCreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 1024, L"foo");
dlls/kernelbase/tests/process.c:566:    ptr = pMapViewOfFileFromApp( mapping, PAGE_EXECUTE_READWRITE, 0, 4096 );
dlls/msvcr120/tests/msvcr120.c:61:            MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp90/tests/misc.c:197:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp90/tests/ios.c:609:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
dlls/msvcp90/tests/string.c:137:                                                 MEM_COMMIT, PAGE_EXECUTE_READWRITE );
```

## R4X O2 uwph_protect + RW-then-RX PE map

- API: `uwph_protect` (x64/uwp-host/include/uwph/mem.h, src/mem.c) refuses
  WRITE|EXEC (EACCES); Linux harness = mprotect, Win32/UWP =
  VirtualProtectFromApp only (needs `codeGeneration`), no RWX emulation.
- Wine: x64/uwp-host/harness/wine-image-wx.patch on dlls/ntdll/unix/virtual.c
  (sites from R4X O1: :2445 header mmap, :3019 image view, :2796 flat images,
  :2951 per-section). Pages are mapped RW, filled + relocated, then set R/RW/RX
  per section. view->protect keeps VPROT_EXEC as the allowed maximum (without
  it set_protection rejects later PAGE_EXECUTE_READ on image pages). Flat images
  and W+X sections fail with STATUS_NOT_SUPPORTED (no RWX fallback).
- Build: harness/prepare-wine-wx.sh rebuilds only ntdll.so. It must match the
  Debian package: install-paths/tmpdir/virtual.c patches, and make_requests
  regenerates SERVER_PROTOCOL_VERSION as 1 (upstream 855 -> "version mismatch 1/855").
- Result: hello.exe passes under `uwpjail --wx` THROUGH our launcher -> patched
  Wine loader -> shim -> uwph: 129 uwph_protect backend calls in the main process
  (131 total), 0 refused RWX mmaps, strace shows no PROT_WRITE|PROT_EXEC
  syscall. Stock Debian ntdll under the same jail exits 13.
- The shim's RWX refusal applies to every shim user, so R4H `wine-test` routed
  mode now uses out/wine-wx (stock ntdll + shim exits 13); its controls stay
  stock. wine-test: all four PEs exit 0, still BLOCKED only by wineboot.
- Still RWX elsewhere (not hit by hello): virtual_create_builtin_view
  (:3539/:3552, ELF-embedded builtins) and PE code that asks for
  PAGE_EXECUTE_READWRITE. Both now get EACCES from uwph_protect/--wx instead of
  RWX pages; an ad-hoc PE check showed VirtualProtect(PAGE_EXECUTE_READWRITE)
  failing with error 5 while RX->RW->RX on an image section worked.

## R4X O3 JIT probe under --wx

`make -C x64/uwp-host/harness install-readiness-glue` builds the readiness
`jit_probe.exe` from `src/pe/jit_probe.c` with MinGW when needed and installs
`bin/jit_probe` as a wrapper for this checkout's `out/run_jit_probe`.
The runner uses our launcher, private W^X Wine loader, and uwph shim with a
separate Wine prefix. It checks the PE summary, both RWX refusal markers,
and uwph protection/refusal counters; captured output is complete before parsing.

Exact test: `/workspace/xbox-readiness/bin/uwpjail --wx -- /workspace/xbox-readiness/bin/jit_probe`.
Result: exit 0, `SUMMARY jit_probe pass=7 fail=0`, `RWX_ALLOC=0`,
`RWX_PROTECT=0`; 2254 uwph_protect backend calls and 3 refused RWX requests.
RW allocation, execution after RW->RX, patch/re-execution, 1000 flip cycles,
and separate RW/RX views pass. No verifier or jail changes, no RWX fallback.
This is THROUGH launcher -> Wine's own loader -> uwph on Linux, not stock-Wine-only
or on-device UWP proof; Xbox codeGeneration still requires device validation.
Video: `/workspace/x64-run/videos/R4X-O3.mp4` (free Xvfb display, ffmpeg, 10 s).

## R4X O4: W^X acceptance (2026-10-24)

Exact test: `/workspace/xbox-readiness/bin/uwpjail --wx -- make -C x64/uwp-host/harness wine-test`.
Exit **2**, preserved unchanged: stock control hello_k32/hello exit **13** under
W^X; verifier reports `BLOCKED: control-hello_k32: nonzero exit`. Both routed
hellos exit **0**, through our launcher → Wine's own loader/private patched
ntdll.so → shim → public uwph Linux backend. This is a partial W^X result,
not an overall wine-test PASS. Wineboot attempts remain in routed traces.

routed-hello_k32: PASS exit=0; selected backend open=12,read=603,close=45,mmap=131,munmap=7,mprotect=84; bypass=0; wineboot attempt remains present
routed-hello: PASS exit=0; selected backend open=13,read=643,close=61,mmap=183,munmap=7,mprotect=129; bypass=0; wineboot attempt remains present

Evidence: `harness/out/wine-o4/run-rmVrdlV7/` (video run) and
`run-wYt5ntrF/` (initial exact run). Main-process reports selected by launcher
execve PID; all six selected families have backend entries, zero reported
bypass and matching module calls/backend counts. This does not cover all Wine
imports or direct syscalls. Verifier and jail were not changed.

Megabonk attempt: owned `/workspace/steam-owned/games/megabonk/Megabonk.exe`,
our launcher + same patched Wine loader + shim under `uwpjail --wx`, isolated
copy of initialized fixture, wineboot disabled. 20-second timeout exits **124**.
Player log reaches Unity 2023.2.22f1 engine, D3D11 device and input initialization.
Trace loads native GameAssembly.dll, sets MONO_REFLECTION_SERIALIZER and
MONO_XMLSERIALIZER_THS, and queries MONO_DISABLE_MANAGED_COLLATION. These markers
are not proof that a Mono runtime initialized. No Steam replacement or DRM
modification was used. Game main-process counters may be absent after timeout;
do not treat child reports as game acceptance.

Raw logs/scripts: `/workspace/x64-run/lanes/R4X/O4-*` and `record-O4.sh`.
Free Xvfb display allocated using `-displayfd`; ffmpeg video:
`/workspace/x64-run/videos/R4X-O4.mp4` (38 seconds, 1280x720).
Readiness `/workspace/xbox-readiness/STATUS.md` W^X row updated to PARTIAL.
Separate stock Linux wineserver remains; no on-device UWP/VirtualProtectFromApp
or in-process-server proof. Stock controls are not uwph evidence; no build-only
result is presented as runtime evidence. No verifier weakening, commits or remote
operations.
## R4W O1 wineboot trigger

Source inspection (2026-10-24): installed Debian wine/wine64/libwine is
10.0~repack-6; `/tmp/wine-src/VERSION` identifies Wine 10.0. The existing
source tree lacked programs/wineboot, so that directory was extracted from
Debian's matching `wine_10.0~repack.orig.tar.xz`. Its SHA256 is
e87ff2971c9b2e79fc63341f044d7513e66f3c3782b49609746428caa87c6ee6;
the matching -6 Debian patch archive has SHA256
2a8343e7da627528a348e591fcf4531a9935512d771f896226b5f375d4898679.
Neither inspected source file is targeted by that archive's Debian patches.
Archives and .dsc came from https://deb.debian.org/debian/pool/main/w/wine/.

**Why a copied prefix still starts wineboot:**
`/tmp/wine-src/dlls/ntdll/unix/env.c:2042` tests `!startup_info_size` in
`init_startup_info()`, then calls `build_initial_params()` at line 2044.
That function unconditionally calls `run_wineboot()` at line 1948.
In `run_wineboot()` (line 1594), the decisive existing-session check is
`NtCreateEvent()` at line 1615 and `status == STATUS_OBJECT_NAME_EXISTS`
at line 1616, which jumps to waiting instead of creating a process.
The named event is `\KernelObjects\__wineboot_event` (lines 1596–1597).
Without that existing wineserver event, it attempts `wineboot.exe --init`
via `NtCreateUserProcess()` (lines 1644–1652). Copying prefix files cannot
copy this live wineserver object. No on-disk prefix stamp is checked before
this launch attempt; a completed prefix alone does not satisfy zero wineboot
attempts in a fresh session.

**Separate decision: whether wineboot updates the prefix.**
`/tmp/wine-src/programs/wineboot/wineboot.c:1921` calls
`update_wineprefix(update)` for init or explicit update. The function at
line 1656 gets the prefix directory from `WINECONFIGDIR` (line 1658), opens
`wine.inf`, and obtains its `st_mtime` (lines 1668–1675). At line 1677,
`update_timestamp(config_dir, st.st_mtime) || force` decides whether to run
the INF update. `get_wine_inf_path()` (lines 93–114) selects
`WINEBUILDDIR/loader/wine.inf` or `WINEDATADIR/wine.inf`.

`update_timestamp()` (lines 118–158) opens
`<WINECONFIGDIR>/.update-timestamp` read/write (lines 123–129). The stamp is
a decimal Unix modification time of the selected wine.inf, followed by a
newline, not a Wine version string. An equal parsed timestamp returns false
(line 135); the source also recognizes a leading `disable` (line 134).
Otherwise a successfully written replacement stamp returns true (lines
138–152); a missing stamp is created on ENOENT (lines 141–144). Allocation,
open, or write failures can return false, so a skipped update does not itself
prove a valid prefix. Explicit update forces the update branch. These are
observations only; no stamp or runtime behavior was changed. Copying a stamp
with a different wine.inf mtime can cause an INF update, whereas a matching
stamp only skips that work after wineboot already starts. `system.reg` is
prefix registry data, not the launch-suppression check described above.

O1 verification: the exact requested grep/ls command exits 0. Its grep scans
ntdll/*.c, not ntdll/unix/*.c, so the launch decision additionally required
inspection of unix/env.c. This is source-only diagnosis: no THROUGH-uwph,
launcher, stock Wine execution, or build was performed. No child Wine
process was spawned, code/verifiers changed, or runtime proof claimed.

## R4W O2 build-time prebuilt prefix

`make -C x64/uwp-host/harness prebuilt-prefix` calls the opt-in
`prepare-wine.sh --prebuilt-prefix` packaging path. On this Linux build box it
runs installed Wine 10.0's `wineboot.exe --init` once for a fresh, isolated
win64 prefix, waits for its matching stock wineserver to finish flushing the
registry, checks all three registry files and the Wine-generated stamp, and
publishes `x64/uwp-host/harness/prebuilt-prefix/`. All real files and directories
have write bits removed; chmod never follows Wine's host drive symlinks. The
large generated tree is gitignored. `packaging.json` records the Wine version,
loader/server hashes, wine.inf hash and mtime. The actual `.update-timestamp`
contains `1750921248\n`, matching this installation's wine.inf mtime, as O1
requires; it is not synthesized or replaced with a version string. A lock
prevents concurrent packagers. Existing packages are checked and reused with
zero wineboot invocations; changed identities or stamps fail explicitly.
For a new Wine installation, remove the generated package (restore owner write
permissions on directories first) and rerun this packaging target.

Runtime integration must explicitly set an absolute `WINEPREFIX`, e.g.
`WINEPREFIX=/workspace/wt/R4W/x64/uwp-host/harness/prebuilt-prefix` for this
box. A deployment should bundle the prepared tree and point WINEPREFIX to its
corresponding deployed location (or an authorized LocalState copy if registry
writes are needed). This packaging target is never a runtime dependency and
no runtime script was changed. Linux Wine's host drive symlinks need platform
path mapping before a UWP deployment. Read-only here means filesystem mode
bits, not a security boundary. As O1 explains, these disk files do not suppress
the fresh-session wineboot launch or supply an in-process wineserver: O2 does
not establish runtime zero-child acceptance.

Validation: the exact O2 make/test command exited 0, creating the read-only
prefix and matching stamp. Repeating it exited 0 and printed
`Reused read-only prebuilt-prefix; wineboot invocations this run: 0`.
`bash -n` and `git diff --check` passed. Evidence classification: **build-only,
stock Wine packaging**, not THROUGH-uwph or launcher execution. The Xvfb :99
recording `/workspace/x64-run/videos/R4W-O2.mp4` shows the repeat exact test,
identity and stamp; it is not runtime proof.

## R4W O3 in-process wineserver — BLOCKED

No in-process server is implemented. Remaining runtime spawn:
`harness/run-wine.sh` explicitly backgrounds `/usr/lib/wine/wineserver64 -f -p`
and later invokes that binary with `-k`. Its stock server is outside uwph.
The current launcher (`x64/pe/launcher/run_via_wine.c`) uses `execv()` to
replace the process with Wine's loader; a pthread created before that handoff
would be destroyed. A server must instead be initialized inside the loader
process, with an explicit ready/error handshake before ntdll connects.

Simply invoking Wine's server main on a pthread is insufficient: matching
Wine 10.0 source `server/main.c:222–238` installs process-wide signal handlers
and enters the server loop; `server/request.c:595–666` changes process cwd,
`:829` registers process atexit cleanup, and `:914` exits the entire process
on socket timeout. `server/signal.c:270–317` installs additional process-wide
handlers. Foreground mode avoids the daemon fork at request.c:859, but does
not isolate these effects from the Wine client in the same process. The
server also uses cwd-relative operations during operation (fd.c:1904,2047).
A source-linked server port needs thread-local lifecycle/error handling,
path-independent socket/registry/file operations and signal ownership shared
with ntdll; renaming main or wrapping the executable does not provide that.
The installed server is an executable, not a thread-callable server library;
no configured Wine source build or embedded server library is available here.
The socket protocol must remain Wine's own during such a port.

The exact requested O3 test was run. Default make succeeds but only builds
`out/libuwph.a`; it neither builds nor runs the Wine test. The unchanged
verifier takes its first argument as an evidence directory, so it reports:
`Wine uwph verification: BLOCKED: [Errno 2] No such file or directory: '--expect-no-children/identity.json'`.
The final grep exits 1 with no matches. No child_processes=0,
wineboot_attempts=0 or PASS was observed. This is a verifier interface
mismatch, not evidence about guest execution. Per the order, the verifier
was not modified. Independently, Wine's fresh-session wineboot path remains
`dlls/ntdll/unix/env.c:1594,1948` (see O1); O2's stamp does not suppress it.

Evidence classification: **build-only and source inspection**, no THROUGH-uwph
runtime proof and no stock Wine execution in O3. Recording
`/workspace/x64-run/videos/R4W-O3.mp4` shows the blocked exact test only.
No runtime code was changed, no commits created and no remotes accessed.

## B15

O1 reviewer fixes: SIGTERM/SIGHUP now run cleanup, including Wine helpers
that start new sessions (identified by this lane's private prefix). Those
helpers are also sampled. An exited private wineserver aborts before client
launch instead of allowing an uncounted existing server. Runs retain separate
timestamped evidence, including a copy of their summary. elapsed_s, max_gap_s
and cadence expose delays rather than guaranteeing an exact 120-second bound.

Exact test `bash x64/tools/memfoot.sh && column -t x64/tools/memfoot.tsv`
exited 0. hello peak 17.79 MiB and hello_k32 peak 17.84 MiB fit both budgets.
Megabonk aggregate peak RSS 1489.55 MiB, estimated commit 1949.60 MiB:
exceeds 1024 MiB App, fits 5120 MiB Game. The actual game observation was
120.254 seconds / 121 samples, maximum gap 1.123 seconds (cadence ok).
Both hello stages exited 0 with two samples, one transient read error each;
their sampled peaks are mostly wineserver because the clients are short-lived.
Megabonk timed out as intended with no read errors. Steam skipped: B10 trace
is absent. No credentials or tokens accessed.

THROUGH our launcher -> Wine's own private W^X loader -> selected uwph shim
calls/Linux backend; separate stock wineserver and private-prefix helpers
included. Not stock-only, build-only or on-device UWP proof. Linux commit is
an estimate from accountable smaps mapping Size, not Windows/UWP commit.
RSS uses smaps_rollup. Shared RSS may double count; sampled peaks are lower
bounds. No optimization or gameplay/authentication success claimed.

Top three individual sampled RSS consumers (maxima are not summed):

- Megabonk.exe (stage Megabonk, PID 685215): 1363.31 MiB.
- explorer.exe (stage Megabonk, PID 685254): 84.63 MiB.
- UnityCrashHandl (stage Megabonk, PID 685290): 26.26 MiB.

Matching raw samples, logs, routing counters, top.tsv and summary.tsv:
`/workspace/wt/B15/x64/uwp-host/harness/out/memfoot/20261025T063410.895220Z-685138`. Summary: `x64/tools/memfoot.tsv`.
SIGTERM and SIGHUP interruption tests passed with no remaining private-prefix
processes; an existing-server test failed before client launch as expected.
The latter test has its own evidence directory and did not overwrite this run.
Video `/workspace/x64-run/videos/B15-O1.mp4`: free Xvfb via -displayfd,
55-second partial view of the measurement. No commits or remote operations.
## B18

O1 (2026-10-25): remaining RWX inventory after R4X. Linux/off-device proof is
THROUGH our launcher -> Wine's own private loader/Unix + PE ntdll -> shim ->
public uwph for the selected host calls. An explicit separate stock Linux
wineserver remains; this is not in-process-server or on-console acceptance.
No RWX exception was added to uwpjail, the shim, or uwph.

The pre-fix inventory (`/workspace/x64-run/lanes/B18/O1-fast-{hello,d3d11,megabonk}.*`,
hello is `O1-hello.*`) used `uwpjail --wx` with
`strace -f -e trace=mmap,mprotect`. The initial all-success stack-unwinding runs
(`O1-{d3d11,megabonk}.*`) timed out during initialization and are not graphics
acceptance. Supplemental `O1-caller-d3d11.strace` uses
`-k --status=failed` to identify the denied kernel request as
`libc.__GI___mmap64 -> libvulkan_lvp.so+0x1f6287` (Mesa lavapipe).
The shim now immediately logs refused calls with PID, operation, caller module
and offset: those calls never enter the kernel and cannot appear in strace.
Counts must include both sources; MAP_DENYWRITE on an RX file mapping is not
PROT_WRITE and is excluded.

The two Megabonk requests initially looked like Unity requests but are Wine
missing-import stubs. `O1-gdb-megabonk.log` records the launched process under
the same jail/launcher/private loader/shim with read-only stack inspection:
Wine syscall frame saved guest SP -> PE ntdll+0x42e0c -> `allocate_stub`
(`addr2line`, Wine loader.c:454), then Unix NtAllocateVirtualMemory -> map_view
-> anon_mmap_alloc. The debug run also tries anon_mmap_fixed because GDB changes
address layout; its four attempts are supplemental, not summed into the baseline
counts. `O1-origin-megabonk.log` independently shows the two 64 KiB
NtAllocateVirtualMemory calls with protection 0x40 (PAGE_EXECUTE_READWRITE).
These are loader-generated unresolved-import stubs, not a Unity JIT.

`wine-loader-stub-wx.patch` fixes that owner: each stub starts RW, is filled,
protected RX, flushed with NtFlushInstructionCache, then published. No shared
page containing earlier executable stubs is reopened writable. Allocation or
protection failure keeps Wine's failure sentinel; the original maximum stub
count stays intact. Cost: at least one page per unresolved import instead of
packing stubs in one 64 KiB pool. `prepare-wine-wx.sh` now rebuilds and installs
private PE ntdll.dll as well as Unix ntdll.so; installed Wine is untouched.
Wine PE build dependencies and matching installed locale.nls are included in
that private build. The focused `stub_probe.exe` imports two nonexistent
version.dll functions, never invokes them, and verifies separate RX allocations.

Inventory (counts are RWX requests per run: shim `UWPH_RWX` lines, which never
reach the kernel, plus kernel-denied `PROT_WRITE|PROT_EXEC` lines in strace;
the two sources do not overlap). Pre-fix = `O1-hello.*` and
`O1-fast-{d3d11,megabonk}.*`; post-fix = `O1-v3-{hello,stub,d3d11,megabonk}.*`
(fresh prefix copies, rebuilt ntdll from 01:07); callers from
`O1-caller-d3d11.strace`, `O1-caller3-megabonk.strace` (`strace -k
--status=failed`) and `O1-gdb-megabonk.log`.

| caller | count | fixed | reachable-on-console |
|---|---|---|---|
| Wine PE ntdll `allocate_stub` (loader.c:454, missing-import stubs) -> NtAllocateVirtualMemory(PAGE_EXECUTE_READWRITE) -> Unix ntdll anon_mmap_alloc (`ntdll.so+0x5dc1a`) | Megabonk 2 before, 0 after; stub_probe 0 after (two stubs, both RX) | yes: `wine-loader-stub-wx.patch` (RW, fill, RX, flush, publish) | yes: any game with an unresolved import (Megabonk has them) |
| Mesa lavapipe `util/u_execmem.c` init_heap, 10 MiB mmap prot 7 (`libvulkan_lvp.so+0x1f6287`), reached via DXVK -> winevulkan | d3d11_probe 1 before, 1 after | no (third-party, denied by the jail; u_execmem returns NULL and Mesa uses its non-rtasm path; readback still 0,128,255,255) | no: Linux-host Mesa software Vulkan; not in the console package |
| Mesa libgallium `u_execmem` same 10 MiB heap (`libgallium-25.0.7...so+0x83ab47`), reached via wined3d GL/llvmpipe | Megabonk 1 before, 1 after | no (third-party, denied, non-fatal; Megabonk reaches Unity `LocalizationPrewarmer:Start`) | no: Linux-host Mesa software GL; not in the console package |
| DXVK 3.1.1 d3d11.dll/dxgi.dll (PE, CPU side) | 0 | n/a | yes (no RWX seen) |
| Unity: UnityPlayer.dll + GameAssembly.dll (Megabonk is IL2CPP/AOT, no JIT) | 0 in the 45 s window | n/a | yes (no RWX seen) |
| hello.exe, launcher `run_via_wine`, shim, uwph, Wine image mapping (R4X patch) | 0 | n/a (R4X) | yes |

Post-fix runs: hello exit 0; stub_probe exit 0 ("PASS: two missing-import
stubs published on separate RX allocations"); d3d11_probe exit 0 with DXVK
native d3d11 ("d3d11 probe done"); Megabonk killed by the 45 s timeout while
running (no crash). Earlier `O1-fixed-*`/`O1-final-*` d3d11/Megabonk runs
crashed explorer.exe in `RtlUnicodeToMultiByteN`. They used a PE ntdll.dll
built before the locale.nls fix and are superseded. The current build passes
d3d11 on both fresh and reused prefixes, with and without strace.
Not covered: winevulkan with a real GPU ICD, and Unity paths after the menu.
The console graphics path is unchanged here; any console Vulkan/D3D driver
needs its own inventory.
## R4W O5 Wine 10.0 server as a static library (build-only)

`x64/uwp-host/wineserver/` (`Makefile`, `build.sh`, `README.md`) builds Wine
10.0's server sources from `/tmp/wine-src` (Debian 10.0~repack), with no
changes, into `x64/uwp-host/wineserver/out/libwineserver.a` (42 objects). The
out-of-tree configure lives in `/workspace/wine-build`
(`--enable-win64 --without-x --without-freetype --without-mingw` and the other
optional libraries off); neither it nor `out/` is committed. Configure runs
with `--no-create`, then `config.status include/config.h`. A full
config.status run fails in makedep because the Debian repack strips the binary
test resources (`dlls/amstream/tests/rsrc.rc: test.avi`), and the server needs
only config.h. Compile flags are configure's CC/CFLAGS/EXTRACFLAGS plus
makedep's `server/` defines (`-D__WINESRC__ -DWINE_UNIX_LIB`, unicode.c
BINDIR/DATADIR), with `-fPIC` added. `main` is renamed with
`objcopy --redefine-sym main=wine_server_main`. The archive has
`T wine_server_main` and no `T main`, and it links into a dummy host with only
libc. The dummy host was linked and never run.

Protocol: `SERVER_PROTOCOL_VERSION 855` appears in
`/tmp/wine-src/include/wine/server_protocol.h` and in `out/protocol_version`.
It matches the stock server `/usr/lib/wine/wineserver64` ("Wine 10.0", dpkg
`wine64 10.0~repack-6`), which is the same Wine release and Debian repack.

Evidence: **build-only**. No server logic was changed and nothing was run,
either through uwph or through stock Wine. Video:
`/workspace/x64-run/videos/R4W-O5.mp4`.
The archive's undefined symbols still include `fork`, `setsid`,
`chdir`/`fchdir`, `atexit`, `exit`, `sigaction` and `ptrace`. These are the
process-wide effects listed under O3, and O6 has to contain them.

## R4W O6 hosted server thread

`wineserver/server_thread.c` starts Wine's own `wine_server_main(-f -p)`
on a pthread. `build.sh prepare` copies server C/headers under ignored `out/`
and applies `patches/0001-hosted-lifecycle.patch`; the original `/tmp/wine-src`
is untouched. The forced `patches/hosted.h` redirects server-only exit/_exit
and atexit to a thread-local callback stack and pthread_exit. Normal and fatal
shutdown run the socket cleanup; the retained lock fd is closed there rather
than relying on process exit. A single server lifetime per process is supported;
Wine's global objects and all allocations are not reset for a second start.

Hosted main does not alter stderr buffering or install signal dispositions.
Hosted init_signals creates only a Wine event-loop pipe handler for explicit
shutdown; watchdog alarms are disabled. SIGPIPE is blocked on the server
thread only. Stop writes to the control pipe; its callback invokes Wine's own shutdown_master_socket(), which drains/kills
its remaining clients and then flushes the registry and exits on the server
thread through Wine's normal socket timeout. Readiness is a byte on a CLOEXEC pipe after
socket listen and all main initialization, before main_loop, with EOF on startup
failure. It does not use a fixed startup sleep or spawn a separate server.

O6 uses the explicitly permitted retained-cwd policy: Wine keeps its original
config/server directory fds and its existing temporary fchdir operations in
fd.c, registry.c and mapping.c. Start must precede client threads; all host and
client filesystem paths must be absolute while the server runs (including the
prefix, executable and any host resources). Relative cwd-dependent host I/O is
unsupported during this lifetime. The shim restores the caller's saved cwd after
join. This policy is sufficient for this isolated protocol test; this step does
not claim arbitrary cwd-dependent applications are integrated into the loader.
The WINEPREFIX environment update likewise occurs before clients start.

`test_host --prefix ABSOLUTE_PATH --selftest` makes a disposable writable copy
of the prebuilt prefix, starts the hosted server and consumes readiness, then
runs one stock `WINESERVER=/bin/false WINEPREFIX=<copy> wine cmd /c echo
R4W-O6-OK`. It checks /proc for wineserver64 throughout, sends the pipe stop
request, joins, restores cwd and removes the copy. A pre-existing unrelated
wineserver64 also fails that strict acceptance check; it is never killed by
the test. Evidence is **our uwp-host server shim + an expressly allowed stock
Wine protocol client**, not THROUGH-uwph PE launcher/loader integration (O7),
and not an Xbox/UWP runtime proof. No verifier changes.

O6 final exact test: `R4W-O6-OK` and `server thread joined rc=0`, but exit 1
because an unrelated existing `/usr/lib/wine/wineserver64 -p0 -p 180`
(PID 339142, PPID 53) violates the global no-server-process gate. No unrelated
process was terminated. Status is BLOCKED, not DONE. Final output and details:
`/workspace/x64-run/lanes/R4W/R4W-O6.result`; proof video:
`/workspace/x64-run/videos/R4W-O6.mp4` (under 60 seconds). Final normal
shutdown leaves no registry-save errors. No commit or remote operation.

## R4W O7 loader integration — BLOCKED on matching Wine protocol

`make -C x64/uwp-host/harness wine-test-inproc-build` links the O6 archive
and server thread host into the Wine routing shim and a separate control-only
library. `run-wine.sh --inproc-server` copies the read-only packaged prefix into
a fresh writable evidence directory and never backgrounds a stock server.
A gated constructor starts only in wine64 after Wine's normal loader re-exec
(`WINELOADERNOEXEC`), waits for readiness before ntdll connects, and marks the
owner so descendants connect to that server. Launcher/control execution remains
separate from THROUGH-uwph attribution. Process exit tears down the server
thread; no destructor join is attempted while ntdll may still be connected.

The prerequisite rerun initially produced O6's expected echo/join but failed
the global server-absence check. Investigation found a more substantial issue:
the installed Debian ntdll uses Debian's `fixes/temporary-directory.patch`, while
the original private server copies used upstream `/tmp/.wine-UID`. Thus earlier
O6/O7 runs could silently connect the client to a different stock server. O7
now applies the server portion of Debian's own patch and its helper header to
private copies only, preserving attribution. No installed binary/source is
modified. After this correction, the O6 client and both O7 modes connect to the
hosted server but fail closed with `version mismatch 855/1`.

Correction to O5 above: identical Wine release strings do NOT prove protocol
identity. The archive/source header reports 855, but installed ntdll expects 1.
No protocol constant or version check has been changed to mask this mismatch.
The O6 prerequisite remains failed (readiness PASS, protocol error, join rc=0,
client/absence combined FAIL). O7's exact make target returns 2; independent
`verify_wine.py` returns 1: `control-hello_k32: nonzero exit`. The verifier is
unchanged, including its wineboot check and its hard-coded separate-server
summary (the latter is not a measurement of this mode).

Final traces show the constructor/server thread in the same loader PID and no
exec of wineserver/wineserver64. Neither PE reaches its hello output against the
hosted server: this is attempted THROUGH-uwph integration, not successful guest
execution proof. Earlier hello successes used a separate server and are not O7
acceptance. O8 event creation and O9 jail changes were not implemented. Video:
`/workspace/x64-run/videos/R4W-O7.mp4` (free Xvfb display, failed exact test).

## R4W O8 prebuilt-prefix startup event — BLOCKED on O7 protocol mismatch

The hosted start now validates the prefix `.update-timestamp` against the
installed `/usr/share/wine/wine.inf` mtime (the same INF used by O2 packaging)
before changing cwd, opening the server socket, or creating its thread. It accepts
Wine CRT CRLF and native LF decimal stamps; missing, malformed, and stale stamps
fail with EINVAL and an explicit instruction to rerun `make prebuilt-prefix`.
The private-source directory patch creates Wine's own permanent manual-reset,
signalled `\KernelObjects\__wineboot_event` during namespace initialization,
before the ready handshake. Retaining the event for the hosted server lifetime
models an already completed packaged-prefix boot. No verifier or wineboot binary
was changed.

The archive and host shim libraries build. Direct host-entry negative checks for
missing and `0` stamps return EINVAL with ready fd unchanged, before server
startup. The exact O8 test was run: make returns 2, all four launcher cases exit
1, and its verifier reports `BLOCKED: control-hello_k32: nonzero exit`.
The valid CRLF stamp passes startup; the installed ntdll then reports the existing
`version mismatch 855/1`. Because make fails, the exact command's `&&` skips the
explicit verifier/tee, so `/tmp/r4w-o8.txt` is absent and grep fails. An independent
verifier invocation confirms the same BLOCKED result. Evidence is
`harness/out/wine-o4/run-F801pWg2`; video is
`/workspace/x64-run/videos/R4W-O8.mp4` (private free Xvfb, Linux host only).
This is attempted THROUGH-uwph launcher/Wine-loader integration, with no successful
hello or hello_k32 execution proof, no stock-Wine-only acceptance, and no claim
that the wineboot-free gate is green. Resolving the O7 protocol prerequisite is
outside this O8 change.

## R4W O9 real nofork jail acceptance — BLOCKED at shell startup

The external `/workspace/xbox-readiness/tests/uwp_run.sh` now honors the
opt-in `UWPH_INPROC_SERVER=1` by skipping its stock-server concession (including
its sleep). Default behavior and jail flags/filter are unchanged. The external
change is retained as `R4W-O9.uwp_run.patch` in the lane evidence directory.

With that environment exported, the exact O9 build succeeds outside the jail.
The real jail reports `uwpjail: seccomp wx=0 nofork=1 (16 insns)`, then
`./run-wine.sh: fork: Operation not permitted` and `rc=254`. A separate strace
confirms the first shell command substitution at `cd "$(dirname "$0")"`
at run-wine.sh line 4 attempts `clone(... SIGCHLD)` and receives EPERM.
No new run directory is created; the independent newest-directory verifier is
therefore on O8's `run-F801pWg2`, and returns 1 with
`BLOCKED: control-hello_k32: nonzero exit`. This is not fresh guest proof.
Global `pgrep -f wineserver64` is also nonempty before/after the exact run;
existing unrelated servers were not terminated. No separate server is spawned
by the opt-in invocation, as confirmed by the diagnostic exec trace.

Evidence: `/workspace/x64-run/lanes/R4W/R4W-O9.exact.log`,
`R4W-O9.spawn.trace`, and `/workspace/x64-run/videos/R4W-O9.mp4`
(4.14 seconds, free Xvfb display). Build-only PASS; attempted THROUGH-uwph
jail acceptance stops before our launcher/Wine loader. No successful stock-Wine
or THROUGH-uwph execution claim. No seccomp/verifier changes, commit, or remote
operation. O9 remains BLOCKED; removing this first shell spawn alone would not
resolve the harness's subsequent helper spawns or O7's protocol prerequisite.
## B10

O1 **BLOCKED** before Steam installation. Neither readiness prefix contained
steam.exe. An isolated copy of LocalState/pfx was made under
`/workspace/x64-run/lanes/B10/pfx`; the supplied PE32 SteamSetup.exe /S was
invoked THROUGH this checkout's launcher -> Wine own private W^X loader ->
uwph shim/Linux backend. Exit 1 after approximately 2.4 seconds: missing
`C:\windows\syswow64\ntdll.dll`, status c0000135, in experimental WoW64.
Steam was never installed, so no `-silent -no-browser` client run or 180-second
client observation was possible. No login or token access occurred.

Child process names + count in the final recorded installer attempt:
conhost.exe 1 (Wine reports started); SteamSetup.exe 1 child loader launch
(fails before guest entry); wineboot.exe 1 attempted child (disabled via
WINEDLLOVERRIDES, fails c0000135). These are three successful child Linux
wine64 execs, not three successfully initialized guest applications.
steam.exe 0; steamwebhelper.exe 0. A separate foreground stock Linux
wineserver was also started once by the harness, outside routed evidence.
Failed preloader/wine executable probes in install.exec are not extra children.

Top 20 missing imports: **none observed**; guest imports were not reached.
The one observed missing module is syswow64/ntdll.dll (1 fatal diagnostic),
not a missing exported function. No unsupported syscall diagnostic occurred;
this does not establish syscall completeness. surface-trace-steam.tsv records
all observed boundary/failure diagnostics with explicit categories and the
unexecuted Steam phase; it is not a completed Steam surface inventory.

Priorities for follow-on work: (1) supply a legitimate initialized Steam
installation or supported 32-bit installer/WoW64 runtime; (2) rerun the actual
180-second routed client trace and rank observed missing imports/syscalls;
(3) treat Chromium steamwebhelper's multi-process model as a separate UWP
single-process wall, unmeasured here. No bypass or emulated Steam was used.

Last substantive failure: `ShellExecuteEx failed: ... err:start:fatal_error ...`.
The literal last log line is a normal ntdll PROCESS_DETACH return, retained
verbatim in harness/steam-evidence/install.log; install.exec contains process
creation and exits, and install.counters.<pid> proves selected uwph routing.
Only open/read/close/mmap/munmap/mprotect are selected by the existing shim,
with its documented native-fd bypasses. This is off-device THROUGH-uwph
installer evidence, not stock-Wine-only proof, Xbox proof, or Steam success.
Raw lane logs and reproduction script are also in /workspace/x64-run/lanes/B10.
Video /workspace/x64-run/videos/B10-O1.mp4 (30 seconds) shows the routed
installer blocker, not a running Steam client. No commits or remotes touched.

Literal last installer log line: `44367.703:0030:0034:trace:module:MODULE_InitDLL (00006FFFFFC20000,PROCESS_DETACH,0000000000000001) - RETURN 1`.

## B21 O1

Reproduced on x64-spike @5f28e621 (R4S+R4X+R4W merged) in worktree
/workspace/wt/B21: `x64/uwp-host/harness/run-acceptance.sh` still fails, rc=1,
`hello_k32 constraints=nofork,noproc,wx exit=13`, Wine prints
`wine: failed to load start.exe: c000000d`. (Setup notes: in a fresh worktree
the very first run fails earlier, rc=2, because the Makefile rule for
`out/wine_linux_backend.o` has no `mkdir -p $(@D)`; also `out/wine-o4/prefix`
must be copied from `/workspace/megabonk/pfx-stock` as run-wine.sh does.)

**Failing call.** Rerunning the same invocation with `WINEDEBUG=+virtual,+module`:

    err:virtual:map_fixed_area mmap error Permission denied for 0x140000000-0x140006000, unix_prot 0x7
    err:virtual:try_map_free_area mmap() error Permission denied, range 0x100000000-0x100006000, unix_prot 0x7.
    err:virtual:map_view anon mmap error Permission denied, size 0x16000, unix_prot 0x7
    (same three again for start.exe, 0x43000 / 0x53000)
    wine: failed to load start.exe: c000000d

- Offending request: anonymous `mmap(0x140000000, 0x6000,
  PROT_READ|PROT_WRITE|PROT_EXEC (0x7), MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)`
  for the hello_k32.exe image view at its preferred base, then the same RWX
  prot for the free-area retries (size 0x6000, then 0x16000 with alignment
  slack). Errno EACCES. Wine then falls back to start.exe
  (env.c:1973), whose image view hits the same RWX map, and gives up with
  STATUS_INVALID_PARAMETER.
- Call site (Wine 10.0, dlls/ntdll/unix/virtual.c): `map_image_view()` line
  3019 `vprot = SEC_IMAGE|SEC_FILE|VPROT_COMMITTED|VPROT_READ|VPROT_EXEC|VPROT_WRITECOPY`
  -> `map_view()` -> `map_fixed_area()` (anon mmap, ERR at line 1990), then
  `map_free_area()`/`try_map_free_area()` (line 1325) and the `map_view()`
  anon fallback (line 2072); `get_unix_prot()` turns that vprot into 0x7.
- Who rejects it: **the uwph shim, not the seccomp filter.** `shim.c:56`
  (`mmap` wrapper, added by R4X O2) returns EACCES for any WRITE|EXEC before a
  backend or kernel call. `strace -f -e trace=mmap,mprotect -e status=failed`
  around the jailed run shows no RWX mmap reaching the kernel (only Wine's
  normal address-space probes failing with ENOMEM). So with the same shim, a
  run without `wx` (`nofork,noproc`) fails the same way today (exit 13,
  same ERR lines). R4S's "passes without wx" result predates the R4X merge.
  uwpjail `--wx` would reject the same mmap (prot & 6 == 6 -> EACCES) if the
  shim allowed it.
- Why: `run-acceptance.sh` exports `WINE_LOADER=/usr/lib/wine/wine64`, the
  **stock** Debian loader + ntdll.so. R4X's fix (`wine-image-wx.patch`, hunk
  @@ -3016, drops VPROT_EXEC from exactly this vprot and sets per-section RX
  later) only exists in the private `out/wine-wx` tree built by
  prepare-wine-wx.sh, which run-wine.sh/run_hello use but run-acceptance.sh
  does not.
- Diagnostic only (not a fix, acceptance script unchanged): the same jailed
  hello_k32 run with `WINE_LOADER=out/wine-wx/lib/wine/wine64` (stamp
  6a05b8a0... matches this tree's prepare-wine-wx.sh) exits 0 and prints
  `hello from a native x86-64 PE (kernel32 only)` under nofork,noproc,wx.

O2 implication: no new mapping change seems to be needed for hello_k32. The
acceptance run has to reach Wine's loader with the W^X ntdll.so. O2 forbids
editing run-acceptance.sh, and `run_via_wine` only uses its built-in
`/usr/lib/wine/wine64` default when `WINE_LOADER` is unset, so the driver
needs to choose where that selection should live.

Evidence: /workspace/x64-run/lanes/B21/O1-evidence/ (run-acceptance.log,
trace-stock-wx.log, trace-stock-nofork-noproc.log, trace-wxntdll-wx.log,
strace-failed-mmap.txt, scripts). Video /workspace/x64-run/videos/B21-O1.mp4
(20 s, private Xvfb). Linux box only, THROUGH our launcher -> Wine's own loader ->
shim -> uwph inside uwp_run.sh/uwpjail; not Xbox proof.

## B21 O2

Fix for the B21 O1 failure: `run-acceptance.sh` (unchanged) now passes under
its default `nofork,noproc,wx`. Both hellos exit 0. The constraint, the shim's
W^X refusal and the acceptance script are untouched.

- **Where the fix lives: the launcher's loader choice.** O1 showed the RWX
  request came from the stock ntdll.so (`map_image_view` vprot with
  VPROT_EXEC|VPROT_WRITECOPY). R4X's `wine-image-wx.patch` already gives Wine's
  own loader the W^X mapping order (map RW, read/relocate sections, then
  per-section R/RW/RX through mprotect -> uwph_protect, never RWX). The harness
  build of `run_via_wine` now bundles that loader
  (`-DRUN_VIA_WINE_WX_LOADER=<out>/wine-wx/lib/wine/wine64`, and the Makefile
  rule depends on `out/wine-wx/.stamp`). When the uwph shim is loaded
  (detected through its `uwph_shim_report` export; its mmap wrapper always
  refuses WRITE|EXEC) and the requested loader is the stock
  `/usr/lib/wine/wine64`, the launcher execs the bundled W^X loader and prints
  `[launcher] W^X enforced by uwph shim: stock ... maps PE images RWX; using
  bundled W^X loader ...`. The acceptance output shows this line. Any other
  WINE_LOADER is used exactly as given. Runs without the shim (run-wine.sh
  stock controls, inproc-server controls) are unchanged.
  `RUN_VIA_WINE_EXACT_LOADER=1` turns the swap off for negative controls.
  Plain `cc run_via_wine.c` builds (on-device scripts) are unchanged. In the
  UWP app this is the same role: the launcher execs the W^X Wine bundled in
  the app's storage.
- **Stale verifier.** R4X added an `mprotect` counter, so every shim report has
  six `total` rows. `verify_noproc.py` (R4S) still required exactly five ops
  and failed every post-merge run. It now requires all six, each with backend
  > 0, bypass 0 and ntdll attribution. That is stricter than before (as in
  verify_wine.py) and proves the RW->RX flips went through uwph.
- Also fixed: `out/wine_linux_backend.o` rule now does `mkdir -p` (O1 fresh
  worktree rc=2).

Results (Linux box, THROUGH uwp_run.sh/uwpjail nofork,noproc,wx -> run_via_wine
-> Wine's own loader with W^X ntdll.so -> shim -> uwph; separate stock wineserver;
Xbox untested): run-acceptance rc=0. hello_k32 `hello from a native x86-64 PE
(kernel32 only)`, hello TEB/malloc/threads ok. hello_k32 had 84 uwph_protect calls
and hello had 129. Refused RWX mmap/mprotect requests: 0. 20/20 traced bindings.
`make -C x64/uwp-host test` rc=0 (70 passed). run-windows-smoke.sh rc=0 (500
passed). run-noproc.sh rc=0. Negative control `RUN_VIA_WINE_EXACT_LOADER=1
run-acceptance.sh` -> hello_k32 exit 13, `failed to load start.exe: c000000d`.
Stability: 16/16 consecutive acceptance passes. One earlier run inside the
recording xterm had hello_k32 exit=1 (it died early, 1 uwph_protect call). Its
log was overwritten and it was not reproduced in 17 retries (including inside
xterm), so the cause is unknown. Video /workspace/x64-run/videos/B21-O2.mp4.
## B20 O1

Question: which WoW64 mode the installed Debian Wine (wine-10.0, Debian
10.0~repack-6: wine64 amd64 + wine32:i386) gives us, and a syswow64 prefix.
All of this is **stock Wine, outside uwpjail** (no uwph routing in O1).

Install layout: there is no `/usr/lib/wine/i386-windows`; Debian puts 32-bit PE
dlls in `/usr/lib/i386-linux-gnu/wine/i386-windows` (810 files) next to an
`i386-unix` tree (32-bit ELF .so). 64-bit side has
`/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/{wow64,wow64cpu,wow64win}.dll`
and an `x86_64-unix/ntdll.so` containing the WoW64 code paths
(`load_wow64_ntdll`, "starting %s in experimental wow64 mode"). No
`/usr/share/doc/wine*` docs beyond copyright/examples; `WINEARCH` only accepts
win32/win64 (no `WINEARCH=wow64` switch). This is a classic
`--enable-win64` + separate i386 build, not an `--enable-archs` build.

Prefix: `WINEPREFIX=x64/uwp-host/harness/out/b20/pfx-wow64 wineboot -i`
(~27 s) -> `#arch=win64`; `drive_c/windows/syswow64/ntdll.dll` exists
(PE32 i386 Wine builtin, 3372656 bytes), syswow64/cmd.exe etc. likewise.

Mode found, default: **old-style WoW64 (separate i386 loader process)**.
`/usr/bin/wine` prefers `/usr/lib/wine/wine` (32-bit). Running
`C:\windows\syswow64\cmd.exe` gives a Linux process whose exe is
`/usr/lib/wine/wine`, ELF 32-bit i386, mapping
`i386-linux-gnu/wine/i386-unix/ntdll.so`. Even when started via `wine64`
(with WINELOADER pointed at a dir with no 32-bit sibling) the 64-bit ntdll.so
still finds and execs `/usr/lib/wine/wine`. So 32-bit = a separate 32-bit
Linux ELF process: unusable under Xbox `--nofork`/single 64-bit process.

Mode available: **new-style in-process WoW64 works with these same stock
binaries** when no 32-bit loader is reachable. In a private mount namespace
(`unshare -rm`, bind-mount an empty file over `/usr/lib/wine/wine` and
`wineserver32`; nothing system-wide changed) `wine64 C:\windows\syswow64\cmd.exe`
prints `err:environ:init_peb starting ... in experimental wow64 mode` and:
- with `WINEDLLPATH` unset -> `failed to load \??\C:\windows\syswow64\ntdll.dll
  error c0000135` — the 64-bit ntdll looks for 32-bit PE in
  `x86_64-linux-gnu/wine/i386-windows`, which Debian doesn't ship.
  **This is exactly the B10 failure text**, so B10's blocker is the PE search
  path, not only the missing prefix syswow64.
- with `WINEDLLPATH=/usr/lib/i386-linux-gnu/wine` -> 32-bit cmd.exe runs
  (`ver` + echo work) inside ONE Linux process, exe `/usr/lib/wine/wine64`
  (ELF x86-64), mapping x86_64-unix/ntdll.so, x86_64-windows/{ntdll,wow64,
  wow64cpu,wow64win}.dll and i386-windows/{ntdll,kernel32,cmd}.dll. No
  i386-unix .so and no 32-bit ELF loaded. (wineserver is still its usual
  separate process, same as for 64-bit apps.)

Consequence for Xbox/uwph: the uwph route already runs wine64 only, so it lands
in experimental in-process WoW64 automatically; it needs (a) a prefix with
syswow64 (this one) and (b) the i386-windows PE dlls on WINEDLLPATH (on Xbox:
package `i386-windows/` alongside x86_64-windows; no i386-unix needed). "Experimental"
caveat: Wine 10 marks this mode experimental; 32-bit GL/Vulkan/thunks via
wow64win are less mature than old-style. O2 should set WINEDLLPATH in the uwph
run. Video /workspace/x64-run/videos/B20-O1.mp4 (19 s): prefix check,
old-style 32-bit process, in-process wow64 cmd.exe. Probe scripts:
/workspace/x64-run/lanes/B20/o1-*.sh.

## B20 O2

Same SteamSetup.exe as B10 (/workspace/steam-owned/SteamSetup.exe), `/S`, no
login, no token, no Steam patching. Reproduction scripts + small evidence:
`x64/uwp-host/harness/steam-evidence/b20-o2/`; raw logs under
`harness/out/b20/o2/` (gitignored) and /workspace/x64-run/lanes/B20.

**Stock Wine (control):** default Debian `wine SteamSetup.exe /S` into the O1
prefix `out/b20/pfx-wow64` -> rc 0, installs
`Program Files (x86)/Steam/Steam.exe` (4379496 bytes) + bin/, public/,
uninstall.exe. Old-style WoW64 (no "experimental wow64" marker; per O1 the
32-bit PE runs in a separate i386 `/usr/lib/wine/wine` process).
Note: the file is **`Steam.exe`** (capital S, as Valve ships it); the O2 exact
test checks `.../Steam/steam.exe`, which fails on case-sensitive ext4. Nothing
was renamed or linked to satisfy it.

**THROUGH uwph:** `run_via_wine` -> private W^X loader (`out/wine-wx`, no
32-bit `wine` sibling, so ntdll.so picks new-style in-process WoW64) +
`libuwph_wine_shim.so` preloaded, separate stock wineserver64, wineboot
disabled (same constraints as B10). Fresh prefix `out/b20/pfx-wow64-uwph`
(setup-only stock `wineboot -i`).
- Installer, WINEDLLPATH=/usr/lib/i386-linux-gnu/wine: B10's c0000135 is gone;
  SteamSetup.exe runs "in experimental wow64 mode" and **exits 0 having
  installed Steam.exe** in the routed prefix. Its post-install
  steamservice.exe fails (`WS2_32.dll failed to initialize`), and
  winex11.so wasn't found for the 32-bit side (see below). 417834 shim-routed
  open/read/close/mmap/munmap/mprotect calls over 19 per-pid reports.
- Client run 1, same WINEDLLPATH: Steam.exe exits 66 after ~10 s:
  `CRYPT32.dll failed to initialize`. Cause: for a 32-bit builtin, ntdll.so
  loads the 64-bit unix side from `<dll dir>/../x86_64-unix/<name>.so`, i.e.
  `/usr/lib/i386-linux-gnu/wine/x86_64-unix/crypt32.so`, which Debian doesn't
  have (also bcrypt.so, ws2_32.so, winex11.so). Debian's 64-bit
  x86_64-unix .so files do export the wow64 thunks (`__wine_unix_call_wow64_funcs`).
- Fix (harness-side, symlinks only): `out/b20/wow64-dlls/{i386-windows ->
  Debian i386 PE dlls, x86_64-unix -> wine-wx x86_64-unix}` as WINEDLLPATH.
  This is also the **Xbox package layout**: ship `i386-windows/` next to the
  64-bit `x86_64-unix/`; no i386-unix and no 32-bit ELF needed.
- Client run 2 with the overlay: Steam.exe (32-bit) runs the full 180 s cap
  (exit 124), draws its "Updating Steam... Checking for available updates"
  window, loads winhttp/wininet/cryptnet and asks
  `https://cdn.steamstatic.com/client/steam_client_win32`. After 60 s:
  `Download failed: http error 0` -> "Steam needs to be online" dialog until
  the cap. Wine log: `wininet:open_http_connection create_netconn failed:
  12029` (cannot connect); secur32/schannel was never reached. **Same failure
  under the stock-Wine old-style control** (stock-installed prefix, 120 s), so
  this is this box's egress (host curl to the same URL gets 200), not
  uwph/WoW64. Not chased further; logging in is out of scope anyway.

Child processes (Windows names, routed): installer: SteamSetup.exe 1,
conhost.exe 2, explorer.exe 1, winemenubuilder.exe 2, steamservice.exe 1
(fails init). Client: Steam.exe 1, conhost.exe 1, explorer.exe 1;
steamwebhelper.exe 0 (never reached, because the client package never
downloaded). Each Windows process is a separate **x86-64 wine64** Linux process
(strace: wine64 execve 5 installer / 4 client), with **zero i386 ELF
execs**: 32-bit code stays inside the 64-bit process. So old-style WoW64 is
NOT required. The remaining Xbox single-process issue is Wine's ordinary
one-Linux-process-per-Windows-process model (explorer/conhost/steamservice,
and later Chromium steamwebhelper), not 32-bit support.

Missing imports: no unresolved guest import seen. Missing exports/modules
logged as warnings: ole32!DllCanUnloadNow, kernel32!CreateMutexWW (installer
probe), winemac.drv, CRYPTBASE.dll, CLBCATQ.dll, Steam crashhandler.dll
(optional). `surface-trace-steam.tsv` (rewritten from B10's placeholder, 62
data rows) lists these per phase, plus child processes, Linux execs, shim
counters, last log lines and the bootstrap errors; generator
`steam-evidence/b20-o2/o2-trace.py`. Last substantive client line:
`[...] Error: Steam needs to be online to update.`; literal last Wine line is
the normal ntdll PROCESS_DETACH.

Video /workspace/x64-run/videos/B20-O2.mp4 (60 s, Xvfb :122): routed client
run 2, the 32-bit Steam updater window under uwph. Run 1 (installer + crypt32
failure) is kept as /workspace/x64-run/lanes/B20/B20-O2-run1.mp4.
