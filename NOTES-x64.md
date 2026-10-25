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
