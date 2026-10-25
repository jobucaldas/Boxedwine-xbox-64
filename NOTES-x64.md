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

## R4G O1

Documented the exact dzn and isolated DComp commands and cited retained stock
Wine logs in `x64/graphics/RESULT-dzn.md` (raw copies under
`x64/graphics/evidence/R4G-O1/`). O1 evidence check PASS; runtime BLOCKED:
DCompositionCreateDevice returns E_NOTIMPL (0x80004001), NULL device; dzn
instance succeeds but physical-device enumeration returns -3 (process exit 10).
Mesa 25.1.9 source root `/workspace/xbox-readiness/dzn/src/mesa-25.1.9`:
`src/vulkan/wsi/wsi_common_win32.cpp:954` is the DComp call; NULL maps to
initialization failure at :992. `src/microsoft/vulkan/dzn_device.c:1164`
initializes WSI during device creation, before any requested surface.
Next missing integration piece: composition swapchain (:795), then HWND target
(:802), visual/SetRoot/SetContent/Commit (:806–809), all in that WSI file.
These later dependencies are source-inspected and untested, not measured
failures. A UWP CoreWindow path remains unimplemented. Evidence classification:
cited STOCK WINE only; THROUGH-uwph/launcher not exercised, no new build.
Lavapipe is not Xbox D3D12; no GPU/presentation or console proof. O1 only.

## R4G O2

Added an opt-in, enumeration-only `x64/graphics/dcomp_stub.cpp`: exported
DCompositionCreateDevice returns S_OK and a real reference-counted
IDCompositionDevice for Mesa's NULL-DXGI request. COM identity, unsupported
IIDs, output clearing, lifetime and E_NOTIMPL presentation behavior are unit
tested. All composition operations remain unimplemented; input arrays are
preserved. Build `make -C x64/graphics` exit 0; test
`x64/graphics/tests/run.sh` exit 0 with
`PASS DCompositionCreateDevice HRESULT=0x00000000; COM identity/lifetime/errors; compositor E_NOTIMPL`.
Evidence: `x64/graphics/evidence/R4G-O2/`. Proof is THROUGH our launcher,
Wine's own PE loader and selected ntdll Unix calls routed via uwp-host shim;
stock separate wineserver remains, no full uwph acceptance claim. This is a
Linux stand-in unit proof only: actual dzn enumeration, swapchain/presentation,
UWP deployment/API authorization and Xbox GPU execution remain untested.
No platform DComp API is invoked by the compatibility DLL. Lavapipe is not
Xbox D3D12. No visual output/video for this textual unit test. O2 only.

## R4G O3

Off-device offscreen Vulkan clear/readback PASS (2026-10-24). Added
`x64/graphics/vulkan_clear.c` and `tests/run-clear.sh`; `make -C x64/graphics`
builds/links the x64 PE against the repository's Vulkan headers (exit 0,
-Wall -Wextra -Werror). Direct ICD loading uses the existing local Mesa
25.1.9 vulkan_dzn.dll and official DXIL dependency, plus VKD3D-Proton and the
O2 enumeration-only dcomp.dll. No ICD registration or loader replacement.
Exact runtime: `DISPLAY=:99 x64/graphics/tests/run-clear.sh` (exit 0);
repeat `VK_LOADER_DEBUG=driver DISPLAY=:99 x64/graphics/tests/run-clear.sh`
also exits 0. Logs/hashes/PID-specific reports: `x64/graphics/evidence/R4G-O3/`.

Measured output: physicalDeviceCount=1; vkCreateDevice=0; vkQueueSubmit=0;
vkQueueWaitIdle=0; `red clear readback: pixels=4096 mismatches=0`;
`PASS offscreen clear/readback only; no presentation or Xbox proof`.
A 64x64 RGBA8 optimal image is cleared to red, copied to a coherent staging
buffer, synchronized to host access, and all 4096 pixels checked. No shader,
triangle, window, surface, swapchain or presentation was exercised.

Proof classification: THROUGH our run_via_wine launcher -> Wine's own PE
loader, with selected ntdll Unix calls routed via the uwp-host preload shim
(positive open/read/close/mmap/munmap counters). A separate stock Linux
wineserver and initialized O2 desktop prefix remain dependencies. This is not
full uwph acceptance or an in-process UWP deployment. The guest's adapter name
`Microsoft Direct3D12 (NVIDIA GeForce GTX 470)` is Wine's reported identity,
not hardware evidence: loader diagnostics identify lvp_icd.json,
libvulkan_lvp.so and llvmpipe (LLVM 19.1.7, 256 bits). **lavapipe ≠ Xbox D3D12**.

Proposed STATUS dzn row update text:
`dzn | off-device clear/readback PASS through launcher + selected uwph routing;
4096/4096 pixels correct on lavapipe; presentation/CoreWindow, UWP authorization
and Xbox D3D12 UNPROVEN (lavapipe ≠ Xbox D3D12).`
The earlier O1 E_NOTIMPL wall is passed only by opting into O2's compatibility
object. Its compositor methods remain E_NOTIMPL; this does not resolve WSI.

Artifacts: `/workspace/x64-run/videos/R4G-O3.mp4` (6 seconds) and
`/workspace/x64-run/videos/R4G-O3.png`: Xvfb :177 terminal capture explicitly
labeled SAVED RESULTS FROM LIVE LAUNCHER TEST, displaying real retained output,
not a rendered clear or fake game screenshot. Original executions used :99.
No console access, commits, pushes or remote operations. O3 only.

## R4G O4

On-device D3D12 probe exe for R4D packaging (2026-10-24). Added
`x64/graphics/probe_d3d12.c`, Makefile rule `out/probe_d3d12.exe` and
`tests/run-probe.sh`; README "O4" section carries the R4D packaging snippet
(args, log locations, RESULT line format, exit codes).

Exact test: `x86_64-w64-mingw32-gcc -O2 -std=c11 -Wall -Wextra -Werror
-municode -static-libgcc -s x64/graphics/probe_d3d12.c -o
x64/graphics/out/probe_d3d12.exe -ldxguid` exit 0; file is `PE32+ executable
(console) x86-64`, 82432 bytes, imports KERNEL32/msvcrt/USER32 only, sha256
eaee3624...7219. Log: `x64/graphics/evidence/R4G-O4/exact-test.log`.

Linux smoke of that same binary (hash in artifacts.sha256):
`DISPLAY=:104 x64/graphics/tests/run-probe.sh --frames 3600` exit 0, THROUGH
run_via_wine -> Wine's own PE loader with selected uwph routing (positive
counters in PID reports) and a separate stock wineserver on the O2 prefix.
Output: `offscreen: 64x64 RGBA8 clear expect=0xffff00ff pixels=4096
mismatches=0`, `present: PASS (3600 frames, 640x360 flip-discard HWND
swapchain)`, `RESULT: PASS adapter="NVIDIA GeForce GTX 470" feature_level=11_1`.
That adapter is Wine's reported identity; backend is Wine builtin d3d12
(vkd3d) over lavapipe. **lavapipe ≠ Xbox D3D12**: nothing here proves the
probe runs, creates a device, or presents on Xbox. HWND presentation is
expected to SKIP under UWP; CoreWindow presentation is not implemented.

Video: `/workspace/x64-run/videos/R4G-O4.mp4` (6.6 s, Xvfb :104) shows the
live probe window clearing green -> blue during the run above.

O4 resume verification (2026-10-24): independently reran the exact MinGW
command above, exit 0; PE32+ x86-64 output exists (82432 bytes). Fresh output
and hash: `x64/graphics/evidence/R4G-O4/exact-test-resume.log`. This is
build-only; earlier smoke/video and their hashes remain historical evidence,
not a runtime test of this fresh rebuild. Recreated lane result/commitmsg;
no commit or remote operations.

O4 reviewer fixes (2026-10-25): initialize each EnumAdapters1 output and
check its HRESULT before dereferencing; unexpected enumeration failures log
the HRESULT and exit via RESULT: FAIL. Reject oversized PROBE_D3D12_LOG
values and failed/truncated explicit-path formatting before adding candidates.
Exact MinGW command rerun with -Wall -Wextra -Werror: exit 0, PE32+ x86-64
output exists (82432 bytes). Fresh evidence and SHA256:
`x64/graphics/evidence/R4G-O4/exact-test-review-fix.log`. Classification:
build-only; retained THROUGH-uwph smoke/video are historical and do not
validate this revised binary. Xbox execution remains unverified.
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
## B09 O1 — fake proc snapshots (2026-10-25)

The provider now supplies self status/stat/auxv, meminfo, fd links and fd/task
directory snapshots from uwph's descriptor, mapping and thread registries.
Own numeric PID status/stat and task-stat paths are supported. Unknown proc
paths fail closed. Memory budgets are 1024 MiB by default or 5120 MiB with
UWPH_MEM_BUDGET_MB=5120; other values fail meminfo with EINVAL. Residency and
CPU counters are unknown (zero), and this is not full Linux proc fidelity.
Tests exercise Wine's process.c fill_vm_counters, system.c meminfo and
thread.c get_thread_times parsing, binary auxv termination, descriptor links,
thread lifecycle and mapping accounting with host procfs hidden.

`make -C x64/uwp-host test` exits 0; complete output is
/workspace/x64-run/lanes/B09/O1-make-test.log. Linux provider tests run THROUGH
uwph; Windows backend unit tests run under stock Wine, not on Xbox.

Exact acceptance reproduction (from /workspace/wt/B09):
`DISPLAY=:2 WINEPREFIX=$PWD/x64/uwp-host/harness/out/wine-o4/prefix EXTRA_RW=$PWD /workspace/xbox-readiness/tests/uwp_run.sh noproc "$PWD/x64/uwp-host/harness" -- ./run-wine.sh`.
Output: /workspace/x64-run/lanes/B09/O1-exact-noproc.log.
Both routed hellos exit 0 THROUGH launcher -> Wine's own private W^X loader
-> shim -> uwph. The command nevertheless exits 1: both stock controls exit
1 with `wine: could not load ntdll.so: (null)` and the unchanged verifier
reports `BLOCKED: control-hello_k32: nonzero exit`. This is not an exact-test PASS.

Openat proof: harness/out/wine-o4/run-ZmdWtgjx/routed-hello.exec and
routed-hello_k32.exec contain `strace -f -e trace=execve,openat` output;
neither contains a /proc path. Fresh noproc probe traces are
harness/out/b09-probes/fs_probe.strace and thread_probe.strace, collected
with `strace -f -e openat`; neither contains a /proc path. Their logs report
`SUMMARY fs_probe pass=7 fail=0` and `SUMMARY thread_probe pass=10 fail=0`,
both exit 0 THROUGH the same launcher/loader/shim/uwph route. The thread
probe's sysinfo memory value remains host-derived; the requested fake
meminfo budget is tested separately and does not claim sysinfo coverage.

All these routed processes see empty tmpfs /proc. The separately prestarted
stock Linux wineserver remains outside the namespace and outside these
traces. Therefore no global no-host-proc claim or concession removal is made;
the shared readiness STATUS row is not upgraded. Xbox remains untested.
Video: /workspace/x64-run/videos/B09-O1.mp4 (24.2 seconds, Xvfb).


## B06

O1 adds the next 15 unsupported traced imports after R4S's top 20, in trace
priority order. All use existing public uwph signatures; each now has its own
counter and shim export. Linux uses native libc passthrough, except descriptor
operations use the existing raw-syscall/owned-slot adapter. UWP uses existing
private objects and documented host APIs; no process creation or optional
restricted API was added.

| Symbol | Win32/UWP status |
| --- | --- |
| sendmsg | In-process Unix socket queue, iovecs and descriptor passing |
| fstat | File metadata through GetFileInformationByHandleEx; private object metadata |
| putenv | Private environment table, shares getenv |
| recvmsg | In-process Unix socket queue and descriptor passing |
| setsockopt | Existing in-process Unix endpoint subset; options have no kernel effect; Internet sockets remain unsupported |
| chdir | Virtual cwd for mount paths and in-process socket directories |
| dladdr | Static registered Unix-library ranges/symbols; no arbitrary module discovery |
| execv | Counted ENOSYS: no documented UWP arbitrary process replacement equivalent |
| getrlimit | Virtual NOFILE, STACK, AS, DATA queries; no host limit control |
| isatty | Private descriptor kind; EBADF/ENOTTY for invalid/non-console descriptors |
| pipe2 | In-process byte channels with descriptor flags; Linux raw pipe2 preserves private ownership |
| setbuf | Existing unbuffered private FILE implementation; Linux native FILE passthrough |
| setrlimit | Bounded virtual NOFILE; invalid soft/hard ordering rejected; other host resources counted ENOSYS |
| sigemptyset | Clears private Linux-compatible signal set |
| sysconf | Virtual limits plus GetSystemInfo/GlobalMemoryStatusEx for supported system queries |

`tests/traced.c` covers every added entry and counter in stub, Linux passthrough
and Win32/UWP builds, including socket payload exchange, pipe bytes/CLOEXEC,
FILE use after setbuf, error handling and unsupported limit/process operations.
Exact test `make -C x64/uwp-host test && make -C x64/uwp-host/harness windows-smoke-test`
passes. The Windows full smoke reports 569 passed, 0 failed; its desktop-only
exception test artifact remains separate from the UWP archive.

Surface policy routed imports increase 26 -> 41. Actual launcher -> Wine's own
private W^X loader -> ntdll -> shim -> uwph evidence increases traced THROUGH
entries 20 -> 34 (+14) for each of hello_k32 and hello. execv is bound but unhit
in these successful runs; its direct tests exercise missing-path Linux failure
and UWP ENOSYS. `harness/verify_b06.py` checks real ntdll bindings, single-process
counter evidence, successful PE output and the >=10 delta, writing per-PE
surface reports. Evidence: `harness/out/wine-o4/run-oD5gMon0/`.
The broader existing `wine-test` verifier still fails its all-routed-symbols-hit
condition because execv has zero hits; it has not been weakened. Stock controls
run through our launcher without uwph. A separate stock Linux wineserver is
retained; none of this is Xbox device or complete UWP compatibility evidence.
Video: `/workspace/x64-run/videos/B06-O1.mp4` (private free Xvfb display, 10.67s).
Exact recorded output: `/workspace/x64-run/v2/B06/O1-record.log`.

B06 reviewer fix: Linux socket dispatch now selects the descriptor domain
explicitly through the separately compiled backend adapter. Direct owned-slot
Linux calls preserve private Unix endpoints for sendmsg/recvmsg/setsockopt and
close; Wine's native-fd backend uses libc consistently for socket/socketpair,
bind/listen/connect/accept, shutdown, message calls, options and name queries.
The public API is unchanged. The socketpair regression now creates and closes
both endpoints through uwph on every backend; additional Linux coverage tests
socket/connect/accept producers, payload exchange, options, names and EOF.

Fresh exact test after the fix: exit 0; direct Linux traced tests 120 passed,
0 failed; Windows full smoke 569 passed, 0 failed. Full output:
`/workspace/x64-run/lanes/B06/B06-O1.exact.log`.
Fresh launcher/Wine-loader evidence: `harness/out/wine-o4/run-F8PMFjl7/`;
`verify_b06.py` passes both PEs at THROUGH-uwph 20 -> 34 (+14).
The unchanged broader verifier still rejects execv's zero hit count.
This is Linux off-device THROUGH-uwph evidence, with separate stock Wine
controls/wineserver and desktop Windows smoke; the UWP archive is built,
not executed on an Xbox. Updated video records the test output and fresh
surface verification on a free Xvfb display (10 seconds).
## B12

O1 reran the owned Megabonk executable with `UWPH_CONSTRAINTS=nofork,wx,noproc
MEGABONK_TIMEOUT=45 x64/uwp-host/harness/run-megabonk.sh` on a free Xvfb
allocated with `-displayfd`. Exact output: `MEGABONK reached=unityplayer`;
game exit=1, harness exit=0 (the harness reports the reached stage). The
45-second ceiling was not reached. `wine.log` confirms
`uwpjail: seccomp wx=1 nofork=1 (34 insns)`; `uwp_run.sh`'s noproc path
mounts an empty /proc. All three constraints remained enabled.

B11's order allowed whichever constraints passed and requested the same
`-force-d3d11 -screen-fullscreen 0` game route. Its worktree has no harness
and its lane has no B11-O1.result or B11 video at inspection, so there is no
verified B11 reached-stage baseline to claim an improvement against. B12
adds the missing runnable harness with mandatory nofork+wx+noproc and records
20 selected uwph surface-counter rows from this run. GameAssembly.dll loads
alongside UnityPlayer.dll: this is IL2CPP, so no Mono JIT is needed.

The first errors are blocked wineboot and explorer startup (`c0000017`);
later RWX anonymous mappings fail with Permission denied (`unix_prot 0x7`).
The final window diagnostic says the explorer process failed to start.
Windows.System.DispatcherQueue is also unavailable. No Direct3D initialization,
first frame, or menu was observed; these logs do not isolate one unique root
cause. The two CreateProcessInternalW attempts are attempts, not successful
child processes. No constraint was relaxed to proceed further.

This is off-device THROUGH-uwph: uwp_run.sh -> our launcher -> Wine's own
private W^X loader -> shim -> Linux uwph backend, with the harness's explicitly
retained separate stock Linux wineserver outside the jail. It is not an
in-process server acceptance or Xbox execution proof, nor stock-Wine-only or
build-only evidence. One shim report records open=10431, read=13511,
close=14295, mmap=8993, munmap=7984, mprotect=1526.

Evidence: `/workspace/x64-run/lanes/B12/B12-O1.exact.log` and the fresh run is
`harness/out/megabonk/run-PV5ZZ0fh`. Video:
`/workspace/x64-run/videos/B12-O1.mp4`, 11.7 seconds, showing the routed run's
terminal output and failure (no game window). Shell syntax check passed.
No commit or remote operation was performed.

B12 reviewer fix: removed the multicolour screenshot first-frame predicate and
logo-based menu promotion. Captures are diagnostic only; automatic reporting
now stops at gfx-init until actual game presentation can be established.
This conservatively prevents blank clients, borders, error windows or
unpresented surfaces from being reported as a rendered game frame. The exact
45-second test was rerun with all three constraints on a fresh private Xvfb:
`MEGABONK reached=unityplayer`, game exit=1, harness exit=0. The startup and
RWX mapping failures persist. The fresh video is 11.7 seconds. Per-run reports
and trace.tsv preserve the new counters; the tracked surface table was retained.
`bash -n` and `git diff --check` passed. No first-frame or menu claim is made.

## B19 O1

ROOT CAUSE: Genuine protocol-number mismatch: hosted Wine server is compiled with upstream SERVER_PROTOCOL_VERSION 855; installed Debian ntdll expects 1. The client does not read 1 from the socket.

/tmp/wine-src/include/wine/server_protocol.h:6778 defines 855; /tmp/wine-src/VERSION is Wine version 10.0. dpkg -s wine64 reports Source: wine, Version: 10.0~repack-6. Installed ntdll strings line 4604 is version mismatch %d/%d.

Wine dlls/ntdll/unix/server.c:1583 calls receive_fd(&version). receive_fd at lines 935-970 receives sizeof(obj_handle_t) bytes with recvmsg and extracts the SCM_RIGHTS fd. server_init_process:1593-1599 compares the received version against SERVER_PROTOCOL_VERSION and prints received-version / compiled-client-version, in that order. Wine server/thread.c:313 sends SERVER_PROTOCOL_VERSION alongside the first request fd.

Installed ntdll disassembly: 0x38784 loads received version into esi; 0x38788 compares esi with literal 1; 0x3878b branches to mismatch at 0x39442. 0x39452 loads literal 1 into edx (second numeric printf argument), 0x39457 selects format at 0x6d258, 0x39460 calls the error routine. Thus 1 is a compiled constant, not erroneous received data.

Fresh control-hello: WINEDEBUG=+server, UWPH_INPROC_SERVER=1, LD_PRELOAD=B19/out/libuwph_inproc_server.so, isolated copy of R4W prebuilt-prefix, B19 run_via_wine -> /usr/lib/wine/wine64 -> Wine own loader. Exit 1, server-ready marker and version mismatch 855/1. strace thread 556809 sendmsg and client 556808 recvmsg both carry W\3\0\0 (hex 57 03 00 00 = 855), with SCM_RIGHTS for the same request pipe. No separate wineserver exec appears. Protocol fails before ordinary request/server debug traffic. Logs retained as control-hello.log, control-hello.strace, control-hello.exit.

nm -D --defined-only captured for libuwph_wine_shim.so (shim.nm), libuwph_inproc_server.so (hosted.nm), installed ntdll.so (ntdll.nm). Overlapping global symbols with ntdll: shim (none); hosted (none). See overlaps.txt. This evidence does not support symbol interposition as the cause.

Existing harness/prepare-wine-wx.sh explicitly documents Debian repack regeneration with make_requests restarting SERVER_PROTOCOL_VERSION at 1; its private loader source in R4W also defines 1. Package version alone cannot prove protocol identity; installed machine code and live wire bytes prove this mismatch.

Scope: THROUGH x64/uwp-host hosted server + our launcher + Wine own loader; stock Debian ntdll client control, not stock Wine alone. No routed uwph syscall-backend or Xbox/UWP acceptance claim. No runtime fixes, protocol bypasses, verifier changes, commits, or remote operations.

Evidence directory: `x64/uwp-host/harness/out/b19-o1/` (ignored build output). Video: `/workspace/x64-run/videos/B19-O1.mp4` (terminal evidence replay).

Independent re-verification (second pass): fresh rerun in out/b19-o1/rerun/ (same path: our run_via_wine launcher -> /usr/lib/wine/wine64 -> Wine's own loader, LD_PRELOAD libuwph_inproc_server.so, WINEDEBUG=+server, WINESERVER=/bin/false) again exits 1 with "version mismatch 855/1"; hosted server thread sendmsg carries W\3\0\0 (855); 0 wineserver execs. Overlap recomputed with comm over nm -D --defined-only global symbols: shim/ntdll none, hosted/ntdll none.
Stock-Wine control (NOT uwph, out/b19-o1/stock-control/): installed Debian /usr/lib/wine/wineserver64 sends iov "\1\0\0\0" (protocol 1) with the request fd, and hello.exe runs to completion (rc=0). So Debian 10.0~repack-6 server and client both use protocol 1; /tmp/wine-src (upstream 10.0 headers, 855) is the odd one out.
Implication for O2 (not done here): build the hosted server against a server_protocol.h regenerated the Debian way (perl tools/make_requests, as prepare-wine-wx.sh already does -> 1), or from the exact Debian source tree. Never patch the version check.

## B19 O2

Fix for the O1 root cause: `wineserver/build.sh prepare` now regenerates `wine/server_protocol.h`, `request_handlers.h` and `request_trace.h` into the private server copy with Wine's own `tools/make_requests`, run on a scratch tree holding only upstream `server/protocol.def` and `server/*.c` (no existing header). That is what debian/rules does for the repack, so `SERVER_PROTOCOL_VERSION` comes out as 1, matching the installed Debian ntdll.so and wineserver64. The prepare step asserts the value is 1. `-I$(COPYDIR)` puts the regenerated header ahead of `$WINE_SRC/include`. Apart from the version line, the generated header is byte-identical to upstream's, and `request_handlers.h`/`request_trace.h` are byte-identical too, so the request layout is unchanged. The version check is not patched.

Exact test (`make -C x64/uwp-host/harness wine-test-inproc` + `verify_wine.py`, run `out/wine-o4/run-jPpVvIIe`): control-hello, control-hello_k32, routed-hello and routed-hello_k32 all exit 0. Each logs `[inproc] Wine loader pid=... server thread ready; no separate server process`, there are 0 wineserver execve in the traces, and the verifier prints PASS. A 0.1 s `pgrep -x wineserver64` monitor never saw a server for this run's prefix. The global pgrep was not empty, though: other lanes' live prefixes (xbox-readiness, B21) had their own servers. Evidence: `x64/uwp-host/harness/out/b19-o2/` (ignored). Video: `/workspace/x64-run/videos/B19-O2.mp4`.

## B19 O3

The existing hosted server validates `.update-timestamp` against the installed
`/usr/share/wine/wine.inf` mtime before starting; its directory patch creates
signalled permanent `\KernelObjects\__wineboot_event` only after that validation.
Missing and mismatched stamps both return EINVAL (22), leave ready_fd untouched,
and tell the packager to rerun `make prebuilt-prefix`.

Completed the pre-existing uncommitted `run-wine.sh --nofork` path: a staged
absolute `--run DIR`, `--case routed-hello|routed-hello_k32` and
`--inproc-server` select one guest using Bash builtins/parameter expansion and
plain exec, before the orchestration path's command substitutions. The normal
harness stages the prefix and traces each guest through this same path. Removed
the inherited wineboot.exe disable override and clear inherited DLL overrides;
Wineboot remains available and the named event prevents its attempted launch.
No verifier changes. Prefix preparation and tracing remain outside the jail.

O3 exact command passed (exit 0), evidence `out/wine-o4/run-wG5V4c1j`:
all four guests exit 0; verifier PASS, no wineboot in guest logs or exec traces.
Routed guests go THROUGH our launcher, Wine's own W^X loader/ntdll and uwph's
selected six syscall families; controls use stock Wine plus our launcher and
hosted server, without uwph counters. This is Linux evidence, not Xbox acceptance.
The verifier's fixed separate-stock-server sentence is generic text, not a
measurement of this run; all four logs contain the in-process ready marker.

Additional real `uwpjail --nofork` staged-path checks pass for routed hello and
hello_k32, with nofork=1 and exit 0. Logs: `out/b19-o3/nofork.txt` and guest
subdirectories. They reuse the exact run's writable prefix. An initial attempt
to make extra prefix copies exhausted disk space; only those new proof copies
were removed, then the successful checks reused the existing prefix. Video
`/workspace/x64-run/videos/B19-O3.mp4` (3.2 seconds) records the live jail checks
and explicitly labels the exact verifier output as from the previous run.
O4's full acceptance command/global process check is not claimed here.


## B19 O4 — exact nofork acceptance BLOCKED

Ran the R4W O9 exact command unchanged, with `UWPH_INPROC_SERVER=1`
exported outside it to skip uwp_run.sh's stock-server concession. Build succeeds;
real uwpjail prints `seccomp wx=0 nofork=1 (16 insns)`, then run-wine.sh reports
`fork: Operation not permitted`, rc=254. The exact invocation selects the
multi-guest orchestration branch, whose first command substitution is
`cd "$(dirname "$0")"` (line 45). The staged single-guest `--nofork` branch
from O3 is not selected by that invocation. Further orchestration also needs
subprocesses, so replacing only dirname would not complete acceptance.

No new run directory was created (runs-before.txt and runs-after.txt identical).
The requested newest-dir verifier PASS belongs to the prior O3 run-JRpnFEiG,
not this jailed attempt. O4 did not reach our launcher, Wine's loader, or uwph;
only the build passed. No fresh guest child_processes=0 proof exists. The global
wineserver check is also nonempty: six pre-existing PIDs are listed unchanged
before/after in the log; no other lane's server was stopped.

Evidence: `x64/uwp-host/harness/out/b19-o4/{acceptance.sh,exact-test.txt,
runs-before.txt,runs-after.txt,xvfb.log,ffmpeg.log}`. Free-display Xvfb recording:
`/workspace/x64-run/videos/B19-O4.mp4` (5.583333 s, 274208 bytes). The recording
shows the failed exact test and explicitly labels the old verifier evidence.
No DONE-R4W.inproc marker was created, because O9 requires all acceptance gates.
No runtime, verifier, jail, remotes, or prior evidence was changed.
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

## B13

O2 (2026-10-25): blocked by the incomplete O1 download. The exact
prerequisite check reports:

```text
ls: cannot access '/workspace/steam-owned/games/hades/x64/Hades.exe': No such file or directory
find /workspace/steam-owned/games/hades -iname 'Hades*.exe': no matches
stock Wine baseline: HADES reached=not-started (missing Hades.exe)
THROUGH-uwph attempt: HADES reached=not-started (missing Hades.exe)
```

Stock Wine is installed (`wine-10.0 (Debian 10.0~repack-6)`), but neither
requested game run could start. O1's result records an incomplete download
stopped by its 20 GB free-space guard; existing partial content is unverified.
O2 does not authorize repeating O1. No 90 s stock/DXVK/lavapipe baseline,
no routed launcher/Wine-loader/uwph execution, and no Xbox-device proof occurred.
The status lines above describe prerequisite failures, not runtime stages.
The exact O2 test is **BLOCKED**: both status lines are recorded here, but
`/workspace/x64-run/videos/B13-O2a.mp4` and `B13-O2b.mp4` are absent because
there was no game to capture. No placeholder videos were generated.

## B24 O1 — fresh RWX caller investigation (blocked)

Restored the missing B12 run-megabonk.sh locally; K_TIMEOUT now selects its
existing timeout (45 seconds for the exact command). No runtime policy changed.
Fresh B24 run: harness/out/megabonk/run-QFSDJnAt, wine.log and report.778103;
raw evidence copied to /workspace/x64-run/lanes/B24/evidence/first-run/.
THROUGH uwp_run.sh -> our launcher -> Wine own private W^X loader -> shim ->
uwph Linux backend, with a separate stock Linux wineserver outside the jail.
This is Linux routed proof, not stock-Wine-only, build-only, or Xbox proof.
Video: /workspace/x64-run/videos/B24-O1.mp4 (private Xvfb, <=60 seconds).

Fresh wine.log lines:
```
1:uwpjail: seccomp wx=1 nofork=1 (34 insns)
2:[launcher] exec /workspace/wt/B24/x64/uwp-host/harness/out/wine-wx/lib/wine/wine64 -> /workspace/steam-owned/games/megabonk/Megabonk.exe (handing guest to Wine's loader)
4:0024:err:environ:run_wineboot failed to start wineboot c0000017
```
Fresh report lines:
```
5:total mmap 8997 0
7:total mprotect 1530 0
```
The report's ntdll.so mmap calls/backend counts are 1080/1080 and mprotect
1530/1530. Zero matches for UWPH_RWX, unix_prot, or Permission denied in the
fresh wine.log. The existing shim report_wx logs module + return offset before
rejecting PROT_WRITE|PROT_EXEC, so a refused shim mmap would be visible even
though it never reaches kernel strace. Run exited 1; MEGABONK reached=unityplayer.

Historical B12 final-evidence/wine.log (not fresh B24 evidence):
```
96:0024:err:virtual:map_view anon mmap error Permission denied, size 0x20000, unix_prot 0x7
97:0024:err:virtual:map_view anon mmap error Permission denied, size 0x20000, unix_prot 0x7
```
These identify Wine Unix ntdll's map_view diagnostic, but give no exact mmap
return offset. Fresh B24 did not reproduce that RWX denial, so no fresh module
+ call site can honestly be named. c0000017 persists without the historical
RWX message; its asserted causal relationship is unproven. O1 BLOCKED on
reproducing the denied request on the current merged baseline; O2 not attempted.

Exact-test repeat: run-OleHKkKS/report.781582 also has zero UWPH_RWX,
unix_prot, or Permission denied matches, mmap=8997, mprotect=1530,
c0000017 for wineboot/explorer, and MEGABONK reached=unityplayer.
Full exact-test output: /workspace/x64-run/lanes/B24/evidence/exact-test.txt
(command completed with status 0; harness reports guest exit=1).
Video verified by ffprobe: 30.400000 seconds. bash -n and git diff --check pass.


## B24 O2 — blocked: no remaining RWX caller reproduced

Exact test on 2026-10-25: `make -C x64/uwp-host test && K_TIMEOUT=45 x64/uwp-host/harness/run-megabonk.sh 2>&1 | tee /tmp/b24.log; ! grep -q c0000017 /tmp/b24.log`.
Host tests passed (including 70 passed, 0 failed / 20 traced symbols); final
exact-test exit=1. Guest exit=1, MEGABONK reached=unityplayer; wineboot and
explorer still return c0000017. Run: harness/out/megabonk/run-2eimxvqZ.
Zero UWPH_RWX/unix_prot/Permission denied lines; mmap=8997, mprotect=1530.
Evidence and full exact output: /workspace/x64-run/lanes/B24/evidence/O2/.
Video: /workspace/x64-run/videos/B24-O2.mp4, 10.2 seconds, free Xvfb :0.
THROUGH uwp_run.sh(nofork,wx,noproc) -> our launcher -> Wine own private
W^X loader -> shim -> uwph Linux backend; separate stock Linux wineserver.
This is routed Linux proof, not stock-Wine-only, build-only, or Xbox proof.

Source inspection: Wine 10.0 dlls/ntdll/unix/process.c spawn_process calls
fork at line 439 and returns STATUS_NO_MEMORY at line 479 when it fails;
NtCreateUserProcess calls spawn_process at line 916. uwpjail.c lines 114–124
reject fork/vfork/non-thread clone with EPERM. This is consistent with the
observed child-start errors, independently of W^X. An attempted strace inside
the unchanged jail failed before launcher startup (test_ptrace_seize: fork:
Operation not permitted); it is not runtime attribution proof. Its separate
diagnostic run reached=none and is not the exact test result.

No runtime patch is justified by a reproduced RWX call. Existing image patch
already maps the historical image-view/header sites RW, then applies section
protections. No constraint was relaxed, no child-process emulation, graphics,
or B19 in-process-server work was attempted. O2 remains blocked by missing
caller reproduction and child-process startup under mandatory nofork.
## B22 O1

**Diagnosis: WINEDLLPATH/install-layout gap. No fix applied.** Reproduced with
an i386 build of the existing `x64/on-device/readiness/net_probe.c` through
this worktree's compiled `x64/pe/launcher/run_via_wine.c`, B20's uwph shim and
private Wine W^X loader, in the existing
`/workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64-uwph` prefix.
B22 has no harness/out build; the shared B20 binaries/prefix are used explicitly
by `x64/uwp-host/harness/steam-evidence/b22-o1/repro.sh` (override assets with
B20_HARNESS). Shim source is identical between B20 and B22. This is Linux
harness evidence THROUGH uwph, not an Xbox on-device run or stock Wine proof.
Separate wineserver64 is infrastructure. No Steam binary or login is needed.

Environment: `WINEDEBUG=+winsock,+module,+loaddll`,
`WINEDLLPATH=/usr/lib/i386-linux-gnu/wine` (the original installer setting),
`WINEDLLOVERRIDES=wineboot.exe=d`, `WINESERVER=/bin/false`, and
`LD_PRELOAD=<B20 harness>/out/libuwph_wine_shim.so` on the launcher/loader.
Wine reports experimental wow64 mode and loads builtin WS2_32.dll at
`0x7b250000`. The actual failing attach is
`DllMain(instance=0x7b250000, reason=DLL_PROCESS_ATTACH=1, reserved=0x0022fd24)`.
Its `__wine_init_unix_call()` requests the WoW64 Unix function table;
`get_builtin_unix_funcs(module=0x7b250000, wow=TRUE, funcs=<output pointer>)`
at Wine `dlls/ntdll/unix/virtual.c:642` calls
`dlopen("/usr/lib/i386-linux-gnu/wine/x86_64-unix/ws2_32.so", RTLD_NOW)`.
The loader logs “cannot open shared object file: No such file or directory”.
The corresponding observed system call is
`openat(AT_FDCWD, "/usr/lib/i386-linux-gnu/wine/x86_64-unix/ws2_32.so", O_RDONLY|O_CLOEXEC) = -1 ENOENT`.
The function returns STATUS_DLL_NOT_FOUND (0xc0000135, source-derived),
WS2_32 DllMain returns FALSE (`dlls/ws2_32/socket.c:580-585`), and the guest
loader aborts with STATUS_DLL_INIT_FAILED (0xc0000142), process exit 66.
The output pointer value is not logged; function arguments/status above are
identified from the matching loader source and observed module mapping.

This is the same missing path logged during B20's steamservice.exe attach.
The probe never reaches main/WSAStartup(0x0202, &wd), so it does not reach its
`socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` call. There is no evidence here
of a refused socket family or a missing uwph socket syscall. The missing
file is the **x86-64 Unix companion of an i386 Windows PE**, not an i386 ELF
library: `<B20 harness>/out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix/ws2_32.so`
exists (links to Debian's x86-64 library) and `nm -D` shows both
`__wine_unix_call_funcs` and `__wine_unix_call_wow64_funcs`. O1 deliberately
uses the failing DLL path and does not activate/change the B20 overlay.

Evidence: `/workspace/x64-run/lanes/B22/o1/{probe.log,probe.strace,probe.exit,probe.file,probe.counters.*}`;
small extracted logs are retained beside the repro script. Exec trace shows
our launcher -> private wine64, unsuccessful i386 loader lookup -> wine64
WoW64 fallback, no successful i386 ELF exec. Shim counters prove routing.
Video: `/workspace/x64-run/videos/B22-O1.mp4`, 12 seconds on a free Xvfb
display; diagnostic caption and DLL-init capture, no game/client UI expected.
Exact test: `grep -q '## B22 O1' NOTES-x64.md` -> exit 0 (no stdout).

## B22 O2

Fixed the O1 WINEDLLPATH layout gap in our launcher. The opt-in
`UWPH_WOW64_DLL_ROOT` validates both `i386-windows` and `x86_64-unix`
directories and sets Wine's DLL root to that paired layout. An incomplete
root fails before exec (exit 127); ordinary launcher use keeps its existing
Wine configuration. The repro packages the distro's i386 Windows PEs beside
B20's private W^X loader's x86-64 Unix companions using local symlinks.
No Steam binary changes, W^X relaxation, or i386 Unix loader required.

Proof is THROUGH uwph: B22-compiled launcher -> B20 private Wine W^X loader
with B20's unchanged uwph shim, using B20's installer-created owned prefix.
This is Linux harness evidence, not an Xbox device result. Separate
wineserver64 is harness infrastructure. The i386 net_probe now reaches main:
WSAStartup, bind/listen, connect, accept and TCP payload all PASS (5/0), exit 0.
The previous missing ws2_32.so attach failure is resolved.

`Steam.exe -no-browser -silent` was run through that same route with a
90-second cap. Got as far as bootstrapper self-update: downloading
`https://cdn.steamstatic.com/client/steam_client_win32`. The manifest request
fails with `Download failed: http error 0`; the bootstrapper verifies the
incomplete installation and retries. Next blocker: bootstrap manifest/network
transport failure, not WS2_32 init. No steamclient.dll load or client UI was
observed; no login attempted. No conclusion about later client compatibility.

Repro and trace extractor: `x64/uwp-host/harness/steam-evidence/b22-o2/`.
Trace: `x64/uwp-host/harness/surface-trace-steam-client.tsv` (same five columns
as the existing trace, with real TSV separators). Full logs, exec/open/socket
trace, exit codes, bootstrap log and per-PID routing counters:
`/workspace/x64-run/lanes/B22/o2/`. Video:
`/workspace/x64-run/videos/B22-O2.mp4`, free Xvfb display, 55 seconds,
with a diagnostic caption; no client UI reached.

Validation: launcher compiles with `-Wall -Wextra -Werror`; an invalid paired
root is rejected. Exact O2 test output is retained in
`/workspace/x64-run/lanes/B22/o2/exact-test.log`. Windows smoke uses a newly
initialized dedicated stock-Wine fixture (setup only) and hosts the uwph
Windows code under stock Wine; it is distinct from the THROUGH-uwph Steam
and net_probe proofs above.
Steam hit the cap (exit 124); shim totals: probe 62,241 calls / 5 reports,
client 84,524 / 5 reports. Wine also logs `wininet:open_http_connection
create_netconn failed: 12029`. Existing harness wineboot-init attempts remain
visible (wineboot.exe disabled, c0000135); this order does not claim the
separate no-wineboot runtime acceptance gate passes. No successful i386 ELF
loader exec is present. O2 exact test: PASS, exit 0; make summaries 55/566
smoke, 60/63/70 traced, all zero failures; Windows futex smoke 500 passed,
0 failed.

B22 O2 reviewer follow-up: the collector now creates a unique `o2/run.*`
evidence directory for every invocation, so old per-PID counter reports cannot
contribute to a later run. It passes the actual selected prefix's bootstrap
log to the extractor and saves a byte-for-byte prelaunch snapshot. Only bytes
appended during the client launch enter the trace; replacement/truncation
fails extraction explicitly. Successful exec evidence now joins strace's
unfinished/resumed execve records by PID before checking the return value.
The collector regression fixture covers an alternate bootstrap path, stale
sibling counter reports, zero current reports, successful resumed execve,
failed execve exclusion, and bootstrap truncation rejection.

Final reviewer-fix rerun evidence: `/workspace/x64-run/lanes/B22/o2/run.0L0qbguI/`
(supersedes the fixed-directory counts above). THROUGH-uwph probe: exit 0,
`SUMMARY net_probe pass=5 fail=0`, 62,285 routed calls / 5 reports. Client:
exit 124 at the 90-second cap, 84,535 routed calls / 5 reports; WS2_32 attach
observed. Again reached bootstrap manifest download, HTTP error 0, then
verification/retry; next blocker remains bootstrap HTTP transport. No
steamclient.dll load or client UI observed. An earlier isolated rerun
`run.L9bSKzTz` timed out during probe initialization; its reports are excluded
from this final trace. Video was regenerated with wall-clock capture timestamps
and one encoder thread: `/workspace/x64-run/videos/B22-O2.mp4`, ffprobe 55.0 s.
Collector regression: 1 test OK; shell syntax and diff whitespace checks pass.
Exact O2 command passed, exit 0; complete output:
`/workspace/x64-run/lanes/B22/o2/review-exact-test.log` (55/566 smoke,
60/63/70 traced, Windows futex 500 passed, all zero failures).
These remain Linux THROUGH-uwph client/probe proofs, with separate stock-Wine
Windows smoke; no Xbox-device claim, Steam login, or client modification.


B31 O1: added `x64/uwp-host/harness/run-battery.sh [CONSTRAINTS...]`.
Default battery runs all six readiness probes for nofork, net, cpu6, noproc,
appfs, wx and nofork,noproc,wx through run_via_wine -> Wine's own PE loader
-> LD_PRELOAD shim -> uwph, using the acceptance fixture and explicit stock
Linux wineserver outside the jail. Each stdout row has four TSV fields:
constraints, probe, exit, report_traced_count. Missing/zero trace evidence,
nonzero exit or probe SUMMARY failures fail the run. Per-run logs/reports
are retained under harness/out/battery/run.*; diagnostics mark PASS/FAIL.
The fixture lock descriptor is closed in children so the persistent server
cannot retain it. Temporary report storage remains writable under appfs
and noproc, without changing constraint gates or probes.

Final exact battery command exited 0, 42/42 PASS, all guest exits 0 and all
trace counts positive. Full TSV: /workspace/x64-run/lanes/B31/battery-exact.tsv;
diagnostics: /workspace/x64-run/lanes/B31/battery-exact.log; reports/logs:
/workspace/wt/B31/x64/uwp-host/harness/out/battery/run.bR1Z3vOb/.
An initial exploratory run had noproc/thread_probe exit 124, traced 0,
FAIL (empty log: harness/out/battery/noproc-thread_probe.log); 41/42 passed.
This intermittent timeout is retained as a finding, not masked or retried
inside the runner. It did not reproduce in the final full exact run.
An initial concurrent Windows smoke failed to acquire the owned fixture;
sequential rerun of `make -C x64/uwp-host test && bash
x64/uwp-host/harness/run-windows-smoke.sh` exited 0. Full output:
/workspace/x64-run/lanes/B31/host-smoke-exact.log; Windows futex 500 passed,
0 failed. Battery proof is Linux THROUGH launcher/uwph; Windows smoke runs
uwph Windows code hosted by stock Wine, not the Unix launcher. Build/static
checks are not Xbox-device proof. Video: /workspace/x64-run/videos/B31-O1.mp4
(45 seconds, free Xvfb display, live battery TSV; no Encom).
B32 O2: run-isolated.sh uses a private user/PID namespace whose user runs as mapped uid 0 (host uid mapped by unshare -Urpf); this is namespace-local, not host root or Xbox privilege.

## B29 O1

**BLOCKED, exact test exit 2.** `make clean` now removes the whole
`x64/on-device/out` directory (it used to keep payload, kit, certs and objects).
From that clean directory `make package` passed. `make offline-smoke` then
failed at the combo rung: `STAGE combo hello+hello_k32-nofork,noproc,wx FAIL: exit=1`.
The cause is `x64/uwp-host/harness/verify_acceptance.py:17`
(`assert set(traces) == set(selected)`). After B06, the shim report lists 35
traced names, but the gate's selection has 20. Reproduced 3/3 by running
`out/payload/combo/run` directly. This is the gate change lane B914 O1 is
making. Per the B29 safety rule, the gate was not edited here. Once B914
merges, rebase and rerun the exact test unchanged.

`pack.sh --kit` now also writes `out/SHA256SUMS`, next to `out/kit.zip`.
It covers `kit.zip` plus every extracted member under `out/kit/`, including
the kit's own inner `SHA256SUMS`. Supplementary, not the exact test: a
separate `make -C x64/on-device kit` built
`/workspace/wt/B29/x64/on-device/out/kit.zip` (228,056,394 bytes, 959
members, the same file list as `out/kit/`). `(cd x64/on-device/out &&
sha256sum -c SHA256SUMS)` gave 960/960 OK, exit 0. Top level of the zip:
`KIT-NOTES.txt README.md SHA256SUMS kit-manifest.txt ladder payload/ probes/
test-cert.cer test-cert.pfx`. `make -C x64/uwp-host test` exit 0 (566 smoke;
99/103/120 traced, 35 symbols; zero failures). The kit build and the checksum
check are build-only. In the offline ladder, stage a ran THROUGH the public uwph
Linux backend. No Xbox, UWP activation or device claim. Video:
`/workspace/x64-run/videos/B29-O1.mp4` (51 s, free Xvfb display, shows the
failing exact-test run).
## B07

O1 implements trace priorities 36–50, the next 15 imports after B06. Each has
an entry counter, shim binding and direct regression coverage. No public
function signatures changed; nl_langinfo uses an internal counted adapter.
Linux uses native libc, except descriptor calls retain the existing explicit
owned-slot versus native-Wine-fd backend. dup adopts the new descriptor into
an owned slot and fchdir translates it while holding the namespace lock.
The dlsym resolver bootstraps libc dlsym via versioned dlvsym so interposition
cannot recurse through itself.

| Import | Win32/UWP implementation and limits |
| --- | --- |
| umask | Existing virtual creation mask; Windows ACLs are not changed |
| __getdelim | Private FILE buffer, growing allocation, delimiter and EOF semantics |
| access | Mapped-path metadata and mode checks; write checks honor read-only mounts; no Windows ACL emulation |
| connect | Existing in-process Unix endpoints; Linux native-fd domain stays explicit |
| dlsym | Static registered Unix-library symbols; Linux native lookup |
| dup | Shared private object with independent descriptor lifetime; Linux raw dup plus slot adoption |
| fchdir | Virtual cwd from a private directory descriptor; Linux translated native directory descriptor |
| fork | Counted ENOSYS: UWP has no documented POSIX process-cloning equivalent; Linux native fork |
| getcwd | Virtual cwd; Linux actual process cwd |
| getpid | GetCurrentProcessId; Linux actual PID |
| lstat | Mapped-file/socket metadata; existing UWP backend has no symlink tree; Linux native no-follow stat |
| mkdir | CreateDirectoryW after mount translation/read-only check; mode does not emulate POSIX ACLs |
| mprotect | Existing memory backend, now counted; shim retains uwph_protect validation and RWX refusal |
| nl_langinfo | C/POSIX CODESET (Linux nl_item 14); other UWP items/locales counted ENOSYS; Linux native locale |
| opendir | GetFileAttributesW/FindFirstFileExW; private readdir/closedir use FindNextFileW/FindClose |

Directory descriptors use virtual paths validated with GetFileAttributesW,
not an unrestricted host-directory handle. Enumeration and mkdir remain
inside the mount table. Tests cover created/empty directory lifecycle,
owned-directory fchdir, dup lifetime after closing the original, invalid
handles, delimiter/tail/EOF reads, static/native symbol resolution, C codeset,
missing parent, native fork/reap, read-only protection, and the existing RWX
gate. Protection tests exercise uwph_mprotect directly for read-only pages;
strict uwph_protect still requires FromApp availability and is not relaxed.

Exact command `make -C x64/uwp-host test && make -C x64/uwp-host/harness windows-smoke-test`
exits 0. Stub smoke: 55 passed, 0 failed; normal Windows smoke: 566 passed,
0 failed; traced tests: stub 139/0, Win32/UWP 154/0, Linux passthrough 172/0
(50 counted symbols each); desktop full Windows smoke: 569 passed, 0 failed.
Full exact output: `/workspace/x64-run/lanes/B07/B07-O1-fix.exact.log`.

Actual launcher -> Wine's private W^X loader -> ntdll -> shim -> uwph runs
both hello PEs successfully. `harness/verify_b07.py` checks all 50 real ntdll
bindings, single-process counters, successful PE output, clean stock controls,
and the >=10 new-entry delta. Both reports rise from B06's 34 to 49 (+15);
all 15 new symbols have hits. Evidence: `harness/out/wine-o4/run-qRzznwUS/` (fresh run after review fixes);
surface verification: `/workspace/x64-run/lanes/B07/B07-surface.log`.
The broader `wine-test` still fails because execv is bound but unhit. Its
all-symbols-hit assertion remains intact; only its traced set now includes
mprotect as both a backend operation and a traced import. The static binding
checker likewise requires all 57 shim exports and all 50 counters.

Classification: Linux off-device THROUGH-uwph proof, with separate stock
Wine launcher controls and a stock Linux wineserver. Windows tests execute
uwph Windows code under stock Wine; the desktop exception artifact is separate
from the UWP archive. The UWP archive compiles with APP headers; no Xbox device
execution or full Wine/UWP compatibility is claimed. Video (12 seconds, free
Xvfb display): `/workspace/x64-run/videos/B07-O1.mp4`.

Review fixes. `uwph_dlsym` resolves registry handles (each registered
library and the `dlopen(NULL)` main handle) only from the static registry;
a registered miss fails rather than falling through. Under Linux passthrough,
only handles the registry does not own (e.g. `RTLD_DEFAULT`) reach native
`dlsym`, so registry pointers are never read as glibc link maps. On Win32/UWP,
path stat/lstat/access answer directories from mapped-path metadata
(`GetFileAttributesExW`) instead of a non-directory `CreateFile2`; regular files
keep the handle-based fstat, and access W_OK still honours read-only mounts.
Tests now cover registry/main-handle hit and miss on every backend plus
access/stat/lstat of a newly created directory.

## B08

O1 implements every remaining numbered import in `harness/surface-trace.tsv`:
priorities 51–64. **There are 14, not 15**: the numbered import trace ends at
64 after B07's priority 50. No untraced fifteenth symbol or inline-syscall
site was substituted. The literal 15-import requirement needs another traced
entry or an explicit scope adjustment; the remaining 14 and the >=10 THROUGH
increase are implemented and verified.

| Import | Win32/UWP backend and Linux boundary |
| --- | --- |
| prctl | Virtual per-thread SET_NAME/GET_NAME; other operations counted ENOSYS. Linux native prctl. |
| pthread_key_create | Existing per-thread key/destructor registry; Linux native key ABI. |
| pthread_mutex_init | Existing private mutex; Linux shim preserves native mutex objects. |
| pthread_mutexattr_destroy | Private attribute lifecycle; Linux native attr ABI. |
| pthread_mutexattr_init | Private normal-mutex default; Linux native attr ABI. |
| pthread_mutexattr_settype | Existing private recursive/normal type; Linux native attr ABI. |
| pthread_setspecific | Private thread-local values; Linux native key ABI. |
| secure_getenv | App-local environment (no privileged-execution mode on UWP); Linux native secure_getenv, preserving libc secure-mode filtering. |
| sigaction | Existing cooperative in-process action table; Linux native sigaction. No new UWP synchronous exception hook or alternate-stack delivery. |
| sigaltstack | Query/disable supported; enabling an alternate execution stack returns counted ENOSYS. Linux native registration/query. |
| socket | Existing in-process Unix stream endpoints; INET remains counted ENOSYS on UWP. Linux Wine-fd mode uses native socket. |
| socketpair | Existing private Unix stream pair; Linux Wine-fd mode uses native socketpair. |
| syscall | Existing typed UWP dispatch for gettid/futex/tgkill/tkill/arch_prctl/utimensat; unknown numbers counted ENOSYS. Linux public typed dispatch calls native libc for known arities; Linux-only shim adapter preserves the native register-slot ABI. No raw guest instruction interception. |
| vsnprintf | Host CRT formatting with a separate counter from snprintf; Linux resolves past the shim to prevent recursive interposition. |

Each has a counter and shim export; the binding inventory now requires all
71 exports and all 64 trace counters. Public API signatures are unchanged.
Native pthread mutex/attribute adapters keep Wine objects compatible with
uninterposed libc companion calls. Linux x86-64 tail thunks preserve opaque
TLS pointers and native syscall/prctl register arguments without speculative
C va_arg extraction. Tests cover all 14 symbols on stub, Win32/UWP and Linux,
including recursive mutex lifecycle, native mutex ABI companion calls, TLS
set/clear, signal query/set/restore, alternate-stack ENOSYS, Unix socketpair
payload, invalid domains/signals/syscalls, thread-name roundtrip, environment
lookup and formatter truncation/sizing.

Exact test `make -C x64/uwp-host test && make -C x64/uwp-host/harness windows-smoke-test`
passes (exit 0): stub smoke 55/0, Win32 smoke 566/0, traced stub 174/0,
Win32/UWP 189/0, Linux passthrough 206/0 (64 symbols each); separate desktop
Windows smoke 569/0. Full output:
`/workspace/x64-run/lanes/B08/B08-O1.exact.log`.

Live evidence: `harness/out/wine-o4/run-b08-HKv5v2xG/`. All four control/routed
PE cases exit 0. Both routed PEs go through this checkout's launcher -> Wine's
own private W^X loader -> real ntdll bindings -> shim -> uwph. The new
`harness/verify_b08.py` checks all 64 real ntdll bindings, successful PE output,
clean stock controls, single-process counters and both the >=10 new-entry
hits and >=10 absolute report increase. Both reports rise from B07's accepted
49 to 60 (+11); 13 of the 14 new symbols have live hits. Current old-entry
hits are 47 (startup-dependent dup/fork are unhit); execv and socketpair are
also bound-unhit. Socketpair has direct cross-backend regression coverage;
no PE execution hit is claimed. Surface output:
`/workspace/x64-run/lanes/B08/B08-O1.surface.log`.

The unchanged broader `verify_wine.py` still fails its all-symbols-hit gate;
no existing gate or verifier was relaxed. The first two cold stock-control
attempts printed hello but hit the existing 12-second strace process-tree
cap. The final evidence explicitly starts desktop services with stock Wine
as SETUP ONLY before measuring all four cases; the setup has no uwph report
and contributes no surface counts. This avoids counting fixture startup as
compatibility evidence. Full final collection output:
`/workspace/x64-run/lanes/B08/B08-O1.proof3.log`.

Classification: Linux off-device THROUGH-uwph PE evidence; separate stock
Wine launcher controls and stock Linux wineserver infrastructure. Windows
checks run uwph Windows code under stock Wine; APP-header UWP archive is
build/test evidence, not Xbox-device execution. Video uses a free Xvfb display
and ffmpeg: `/workspace/x64-run/videos/B08-O1.mp4` (12-second labelled replay
of the saved PE evidence and exact-test summaries). No remotes,
commits, credentials, game/client modifications or out-of-scope mechanisms.
## B28 O2

Status: BLOCKED for an in-process desktop prototype; source investigation and
design complete. No runtime change is made by this order.

Spawn site (local Wine 10.0 source, `/tmp/wine-src/VERSION`):
`/tmp/wine-src/dlls/win32u/winstation.c:775` defines `get_desktop_window()`.
It queries the server at lines 792–801, then, if no top window exists,
constructs `C:\\windows\\system32\\explorer.exe /desktop` at lines 805–809.
The actual spawn is `NtCreateUserProcess()` at lines 855–857 (WOW64 branch)
and 861–863 (normal x64 branch). It resumes the thread and waits for process
input idle at 866–870; line 872 emits `failed to start explorer`. The forced
server lookup at 874–883 is a fallback, not proof that explorer initialized.

Wine's `/desktop` parser calls `manage_desktop()` in
`/tmp/wine-src/programs/explorer/explorer.c:896` (quoted option: 902).
`programs/explorer/desktop.c:1197` implements it: graphics-driver setup at
1247–1249, desktop and message-parent windows at 1269–1277, clipboard thread
at 1286, and the desktop message loop at 1326–1332. These are the existing
Wine implementations to reuse, rather than inventing a substitute desktop.

Proposed nofork design:

1. Factor Wine explorer's desktop implementation into a private PE DLL with
   an explicit desktop-thread entry and startup/stop result. Load that DLL
   through Wine's own PE loader in the game process; do not invoke an EXE
   entry point as a thread. Preserve explorer resource/module identity and
   its desktop, appbar, clipboard and driver dependencies (see
   `programs/explorer/Makefile.in:1–14`).
2. Start the worker through a PE-side user32 initialization callback before
   game window creation, outside the loader lock. Bind the worker to the
   caller's window station/desktop before creating windows. Coordinate one
   owner per desktop, with a recursion guard and an explicit ready/failure
   event signaled after both server desktop windows exist. Wait with a
   bounded timeout, then repeat the real server lookup. A thread readiness
   handshake must replace the current process `NtUserWaitForInputIdle`.
   Integrate the win32u spawn branch with that PE callback for explicit
   nofork mode; do not directly call PE user32/explorer code from Unix code.
3. Refactor process-lifetime assumptions first: desktop.c:1260 and 1336
   call `ExitProcess`, and 1265–1267 changes the process token. Worker
   failure/exit must return a status and clean up only worker resources;
   it must not terminate or change privileges of the game. Audit callbacks,
   clipboard and display-restorer threads for shutdown and module lifetime.
   Use a desktop-only configuration without shell launchers, tray services,
   DDE/COM shell startup or command execution; desktop.c:1312 otherwise
   invokes `CreateProcessW`. Preserve window and graphics initialization.
4. Extend the private Wine build/staging to compile the affected win32u,
   PE support and desktop DLL together. `harness/prepare-wine-wx.sh` currently
   builds only Unix/PE ntdll and stages other DLLs from installed Wine; a
   source-only win32u edit would not exercise the changed code.

This spans a new PE module, process-to-thread lifecycle refactoring and
reproducible Wine staging, so it does not fit a small prototype in this
session. Keep nofork/wx/noproc and all gates unchanged. Future acceptance
must rerun `K_TIMEOUT=45 x64/uwp-host/harness/run-megabonk.sh` through our
launcher and Wine loader, show zero `failed to start explorer` entries and
a non-black `shot-*.png`, and record an Xvfb/ffmpeg video <=60 seconds at
`/workspace/x64-run/videos/B28-O2.mp4`. No game run or video is claimed here.

Validation: `grep -q '## B28 O2' NOTES-x64.md && make -C x64/uwp-host test`.
This exercises uwph Linux/Windows library tests (the Windows tests use stock
Wine) and harness checks; it cannot prove an in-process desktop or Xbox UWP
graphics. Record the exact output and exit status in the lane result files.

## B11 O1 — Megabonk THROUGH-uwph (Linux only)

`x64/uwp-host/harness/run-megabonk-wx.sh` (originally B11's
`run-megabonk.sh`; renamed in the B916 merge so B12's nofork,wx,noproc
`run-megabonk.sh` keeps its name) uses the same launcher, private W^X
Wine loader and native-fd uwph shim as `run-hello.sh`, with a private copied
prefix, external stock wineserver, a free Xvfb display and a 120-second guest
timeout. It records at most 58 seconds to
`/workspace/x64-run/videos/B11-O1.mp4` and appends measured wrapper/backend
hits to `x64/uwp-host/harness/surface-trace-megabonk-wx.tsv` (kept apart
from `surface-trace.tsv`, whose priority schema `verify_*.py` read).
The recorder writes a fresh temporary MP4, and the harness waits for its exit,
requires a nonempty video stream with a finite duration of at most 60 seconds,
and decodes the full capture before publishing the required video path.
Recording or validation failures fail the test while preserving the printed
guest-stage report. A recorder that exits during startup fails the run before
the guest launches. The run lock fd is not inherited by Xvfb, ffmpeg,
wineserver or the guest, and cleanup reaps processes left in the private
prefix (an orphaned explorer.exe/UnityCrashHandler64 previously held the lock
and made later runs exit silently).
Counters and diagnostic logs
are in `harness/out/megabonk/`. Only `--wx` is applied: R4W O3 still marks the
in-process server blocked; nofork/noproc are not claimed. This is selected-call
routing on Linux, not an Xbox/UWP device pass or complete syscall coverage.

The owned game ships `GameAssembly.dll` and
`Megabonk_Data/il2cpp_data/Metadata/global-metadata.dat`: it is IL2CPP, with
native ahead-of-time code, so no Mono JIT is needed. No Steam/DRM components
were replaced or modified. The first missing service reported by Unity is
`SteamAPI_Init() failed`; audio also lacks a host device and falls back to
silent software output. Neither prevented the observed main menu. The script
reports the conservative log-proven `gfx-init` stage (D3D11 device created);
manual inspection of `out/megabonk/screen.png` during the same run confirmed
the rendered MEGABONK title, PLAY, SETTINGS and EXIT menu. Gameplay was not
tested. No credentials were read, no remotes accessed, no commit created.

## B33 O1

Spawn inventory and explorer dependency evidence for the constrained Megabonk run follow below.

The added metric counts only `trace:process:NtCreateUserProcess` entry records
with image/cmdline fields, preserving Wine's quoted escaping in `spawn:` lines.
Classification is case-insensitive on the decoded `C:\windows\` image prefix;
the pre-existing CreateProcessInternalW diagnostic-line metric is retained.

Explorer feature inventory (source paths below are relative to `/tmp/wine-src`).
NEEDED means the observed failing path requires it; UNKNOWN means the early
failure/debug channels cannot establish whether the guest will use it later.
Absence of a trace is not evidence of NOT-NEEDED.

| Explorer feature | Status | Evidence and limit |
| --- | --- | --- |
| Desktop window | NEEDED | `dlls/win32u/winstation.c:775,855-883`: get_desktop_window attempts explorer, then forces a bare server desktop after failure. `programs/explorer/desktop.c:1270-1272` creates the normal desktop. |
| Display GUID / graphics driver and desktop message pump | NEEDED | `dlls/win32u/driver.c:953-975` sends WM_NULL, reads `__wine_display_device_guid`, opens its registry key and queries GraphicsDriver. `dlls/win32u/defwnd.c:2962-2986` sets the GUID from desktop creation data; `programs/explorer/desktop.c:1249,1272,1327-1331` supplies the GUID and message loop. Observed nodrv_CreateWindow failure ties this path to the guest. |
| Message window / HWND_MESSAGE parent | UNKNOWN | `programs/explorer/desktop.c:1276-1277` creates it; `dlls/win32u/winstation.c:874-883` also obtains msg_window from the forced server fallback. Existing process/loaddll/err channels do not establish guest message-only window use. |
| Clipboard | UNKNOWN | `programs/explorer/desktop.c:680,1286-1287` defines/starts clipboard_thread. No clipboard API tracing enabled; explorer never starts, so no evidence of guest clipboard use. |
| Display-mode registry | UNKNOWN | `programs/explorer/desktop.c:1090-1130,1290` persists current modes via EnumDisplaySettingsExW/ChangeDisplaySettingsExW. GraphicsDriver registry lookup above is distinct; this run does not establish guest consumption of stored display modes. |
| Systray | UNKNOWN | `programs/explorer/desktop.c:1293` initializes it; no guest Shell_NotifyIcon evidence in these channels before the graphics failure. |
| Appbar | UNKNOWN | `programs/explorer/desktop.c:1291` initializes it; no guest SHAppBarMessage evidence in these channels. |
| Shell DDE | UNKNOWN | `programs/explorer/desktop.c:1296-1300` loads shell32 ordinal 188 / ShellDDEInit. Loading shell32 alone does not establish guest DDE use. |

No optional explorer feature is declared NOT-NEEDED from this early failed run.

Run evidence: `x64/uwp-host/harness/out/megabonk/run-xNTMqtSN/wine.log`
(line numbers below), console `/tmp/b33.log`, filtered inventory/service evidence
`/tmp/b33-spawn-service-evidence.log`. One Megabonk run, private Xvfb allocated
with `-displayfd`; video `/workspace/x64-run/videos/B33-O1.mp4` (9.4 seconds,
diagnostic Xvfb capture including live harness output; no rendered game claimed).

| Attempt | Image and command line (Windows paths) | Classification | Caller | Runtime evidence |
| --- | --- | --- | --- | --- |
| 1 | `C:\windows\system32\explorer.exe`; `"C:\windows\system32\explorer.exe" /desktop` | wine-internal | `/tmp/wine-src/dlls/win32u/winstation.c:860-863`, get_desktop_window | wine.log:48 entry, :49 failure c0000017 |
| 2 | `Z:\workspace\steam-owned\games\megabonk\UnityCrashHandler64.exe`; `"Z:\workspace\steam-owned\games\megabonk\UnityCrashHandler64.exe" --attach 32 140737474269184` | guest | guest (Unity crash handler launch; guest binary source/callsite unavailable) | wine.log:57 command, :59 handle-list diagnostic for the same call, :60 NT entry |

Both NT trace entries originate at `/tmp/wine-src/dlls/ntdll/unix/process.c:785`;
this is the common trace site, not the initiating caller. The retained
`CreateProcess attempts: 2` counts two diagnostics from the single guest call;
the new metric independently reports **2 (wine-internal: 1, guest: 1)**.
The main Megabonk image is loaded by our launcher and is not an NT spawn attempt.
Desktop/driver runtime evidence: wine.log:49 and :107-108 report explorer failure
and no graphics driver, supporting the NEEDED rows above. No successful desktop
GUID/property read is claimed.

On-demand service inventory command:
`grep -nEi 'StartService|services\.exe|rpcss|plugplay|winedevice|combase:RoGetActivationFactory' wine.log`.
Only matches: :103 semi-stub RoGetActivationFactory for
`Windows.System.DispatcherQueue`, and :104 failed to find its library.
There are no StartService/services.exe/rpcss/plugplay/winedevice matches and no
additional NT spawn entries. `/tmp/wine-src/dlls/combase/roapi.c:146-190` resolves
the activation DLL in-process; the missing DLL is not itself a logged service
spawn. These channels do not trace every service API; later service demand
remains UNKNOWN because the guest stops at early graphics initialization.

Exact requested test executed with DISPLAY set to the private Xvfb:
`make -C x64/uwp-host test && K_TIMEOUT=45 x64/uwp-host/harness/run-megabonk.sh 2>&1 | tee /tmp/b33.log; grep -E '^NtCreateUserProcess attempts: 2 \(wine-internal: 1, guest: 1\)' /tmp/b33.log && grep -c '^spawn: ' /tmp/b33.log && grep -q '## B33 O1' NOTES-x64.md`.
Make completed successfully (including 172 passed, 0 failed / 50 traced symbols
and 7 in-process verifier checks passed). Final grep output:

```text
NtCreateUserProcess attempts: 2 (wine-internal: 1, guest: 1)
2
```

Grep chain exit 0; harness reports exit=1 and MEGABONK reached=unityplayer.
Proof is THROUGH-uwph: uwp_run.sh(nofork,wx,noproc) -> our launcher -> Wine's own
W^X loader -> shim -> uwph, with Wine server hosted in-process. Installed stock
Wine ran only the explicitly labelled packaging-time prefix preparation;
it is not the game acceptance proof. Linux only; Xbox untested. The generated
surface-trace.tsv refresh was restored to its original content after the run;
no enforcement gates or constraints changed. O2 was not implemented.

## B33 O2

Added `desk_probe.c` and its mingw build, plus `run-desk-probe.sh stock|nofork`. The probe checks desktop existence, display GUID, a visible 320x240 GDI window with pixel readback, a message-only child and its ancestor, Unicode clipboard roundtrip, and current display width. Systray presence is informational. Every mandatory check contributes to the exit status. The GUI subsystem avoids a probe-induced conhost launch while retaining redirected stdout. An initial console-subsystem experiment reproduced the same desktop failures but also attempted conhost; the final exact test below uses the GUI build and counts only explorer under nofork.

Stock uses an installed Wine and separate server with a private writable copy of the prebuilt prefix. Nofork copies the prefix afresh and runs THROUGH `uwp_run.sh(nofork,wx,noproc) -> run_via_wine -> Wine own W^X loader -> shim -> uwph`, with `UWPH_INPROC_SERVER=1` and `WINESERVER=/bin/false`. Both use `WINEDEBUG=+process,err` and print the identical O1 NT-entry spawn metric. Prefix copies are removed on exit; full logs/reports remain at the evidence paths below. Stock Wine is an explicitly permitted baseline, not THROUGH-uwph proof. Prefix preparation is build-only. Linux proof only; Xbox untested. No constraints or verification gates changed.

Final stock output:

```text
Reused read-only prebuilt-prefix; wineboot invocations this run: 0
Build-only stock Wine preparation; no runtime acceptance claimed
DESKTOP PROBE mode=stock DISPLAY=:0
DESK desktop_window PASS
DESK display_guid PASS
DESK driver_window PASS
DESK message_window PASS
DESK clipboard PASS
DESK display_modes PASS
DESK systray INFO present=1
DESK result PASS
exit=0
NtCreateUserProcess attempts: 11 (wine-internal: 10, guest: 1)
spawn: L"C:\\windows\\system32\\wineboot.exe" L"\"C:\\windows\\system32\\wineboot.exe\" --init"
spawn: L"C:\\windows\\system32\\winemenubuilder.exe" L"C:\\windows\\system32\\winemenubuilder.exe -a -r"
spawn: L"C:\\windows\\system32\\services.exe" L"\"C:\\windows\\system32\\services.exe\""
spawn: L"C:\\windows\\system32\\winedevice.exe" L"C:\\windows\\system32\\winedevice.exe"
spawn: L"C:\\windows\\system32\\plugplay.exe" L"C:\\windows\\system32\\plugplay.exe"
spawn: L"C:\\windows\\system32\\explorer.exe" L"\"C:\\windows\\system32\\explorer.exe\" /desktop"
spawn: L"C:\\windows\\system32\\svchost.exe" L"C:\\windows\\system32\\svchost.exe -k LocalServiceNetworkRestricted"
spawn: L"C:\\windows\\system32\\winedevice.exe" L"C:\\windows\\system32\\winedevice.exe"
spawn: L"C:\\windows\\system32\\rpcss.exe" L"C:\\windows\\system32\\rpcss.exe"
spawn: L"C:\\windows\\system32\\conhost.exe" L"\"C:\\windows\\system32\\conhost.exe\" --unix --width 0 --height 0 --server 0x30"
spawn: L"Z:\\workspace\\wt\\B33\\x64\\pe\\tests\\desk_probe.exe" L"\"Z:\\workspace\\wt\\B33\\x64\\pe\\tests\\desk_probe.exe\" "
00b0:err:ntoskrnl:ZwLoadDriver failed to create driver L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\winebth": c00000e5
stock Wine baseline; separate wineserver allowed; explorer spawned normally
evidence: /workspace/wt/B33/x64/uwp-host/harness/out/desk-probe/evidence-WBpgWShJ
```

Final nofork output:

```text
Reused read-only prebuilt-prefix; wineboot invocations this run: 0
Build-only stock Wine preparation; no runtime acceptance claimed
DESKTOP PROBE mode=nofork DISPLAY=:0
DESK desktop_window PASS
DESK display_guid FAIL
DESK driver_window FAIL
DESK message_window PASS
DESK clipboard PASS
DESK display_modes PASS
DESK systray INFO present=0
DESK result FAIL
exit=1
NtCreateUserProcess attempts: 1 (wine-internal: 1, guest: 0)
spawn: L"C:\\windows\\system32\\explorer.exe" L"\"C:\\windows\\system32\\explorer.exe\" /desktop"
0024:err:win:get_desktop_window failed to start explorer c0000017
0024:err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be loaded.
0024:err:winediag:nodrv_CreateWindow L"The explorer process failed to start."
THROUGH uwp_run.sh(nofork,wx,noproc) -> launcher -> Wine own W^X loader -> shim -> uwph; Wine server hosted in-process
evidence: /workspace/wt/B33/x64/uwp-host/harness/out/desk-probe/evidence-XSKIdx6G
```

The requested exact test ran on a free Xvfb display and exited 0 (`/tmp/b33-exact.log`). `sh x64/pe/tests/build.sh` succeeded; the two expected FAIL greps and `wine-internal: 1` grep succeeded; `make -C x64/uwp-host test` passed (including 172 passed/0 failed shim checks and 7 passed/0 failed in-process verifier checks). Stock passed every required feature. Nofork returned 1 as expected, with explorer refused (`c0000017`), missing display GUID, and no driver window; clipboard/message/display-mode checks still passed. This is the B34–B36 acceptance oracle.

Video: `/workspace/x64-run/videos/B33-O2.mp4` (13.2 seconds, private Xvfb + ffmpeg), showing both runs and the exact-test completion.

## B50 O2

BLOCKED (stock Wine only, not THROUGH uwph): Valve steam.exe never reached logged-on state within 120 seconds after the single seeded-cache attempt. The client remained `[Logged Off, 0, 0] [U:1:0]`; connectivity checks timed out. This does not isolate token rejection from network/UI initialization failure. No alternate credential path or CEF installation was attempted.

Added the public Steamworks probe, stdin-only Wine CryptProtectData helper, private login-state seeder, and stock-Wine runner. Valve/game binaries remain unmodified; the game stage uses symlinks. The probe and game were skipped on the failed-login stop condition, so there is no probe output or Player.log and no engine progress claim. Helpers built with mingw x86_64; Bash syntax and Python compile checks passed.

Flags requested: `-silent -nofriendsui -nochatui -nobigpicture -nointro -noverifyfiles`. Bootstrap log echoes all six; no Steam log calls them unknown/ignored. Acceptance of their semantics is unproven. Process trace lists steam.exe, steamerrorreporter64.exe, steamsysinfo.exe, GL/Vulkan driver queries (32/64), Wine services/conhost/device helpers, and an attempted missing steamwebhelper.exe. Full command lines: `/workspace/x64-run/lanes/B50/o2/processes.txt`.

Exact test exited 1 at run-minimal.sh, so subsequent && assertions did not execute. Required raw pgrep samples: 127, maximum 1; steamwebhelper CreateProcess lines: 1; webhelper_count: 2. The raw global count can include ambient command-line matches and was retained without filtering. Separate required leak check passed (exit 0, no output). Video: `/workspace/x64-run/videos/B50-O2.mp4` (8 seconds). Exact Steam bootstrap/connection logs are preserved in `o2/steam-logs/`; salient lines:

```text
[2026-10-25 17:20:57] Suppressing Steam update
[2026-10-25 17:21:08] [Logged Off, 0, 0] [U:1:0] CCMInterface::SetSteamID( [U:1:0] )
[2026-10-25 17:21:23] Connectivity test (198.18.0.1:80 (198.18.0.1:80)): HTTP fetch failed
[2026-10-25 17:22:50] Connectivity test: result=TimedOut (since 87.4s ago), prev=Unknown, in progress=0
```

Steam UI log: `[2026-10-25 17:20:57] Started webhelper process 0`. Wine trace records the attempted `bin\cef\cef.win64\steamwebhelper.exe` CreateProcess. This does not prove that a helper process successfully started.

## B50 O3

Added `run-minimal.sh --uwph` via `run-wine.sh --guest`: launcher -> Wine own W^X loader -> shim -> uwph, constraints `wx,noproc`, nofork OFF, external stock wineserver concession. Linux only. Valve bootstrap reached “Suppressing Steam update” but no connection log/logged-on state appeared within 120s. Probe and game skipped by the login gate; no SteamAPI or game-progress proof. Webhelper count 0 (127 samples, max 0, matching CreateProcess lines 0). Exact test exited 1. Host tests and Windows smoke command exited 0 (500 smoke checks passed). Evidence and detailed limitations: `/workspace/x64-run/lanes/B50/o3`, `B50-O3.result`; video `/workspace/x64-run/videos/B50-O3.mp4` (8s). No alternate credential path or CEF attempted.

## B34

O2 implementation is **BLOCKED at runtime proof**, not DONE. The new
`harness/wine-desktop-thread.patch` shares Wine's explorer initialization with
`uwp_desktop_start`, using an atomic single owner and one ready event, and
preserves the explorer process path. The desktop thread omits admin-token
changes, launchers, cmdline execution, systray/appbar (B33 UNKNOWN), and Shell
DDE. `desk_probe --self-host` loads and starts the DLL before user32 checks;
`run-desk-probe.sh stock-selfhost` stages the DLL in its disposable prefix,
sets the private loader/DLL path, and uses our launcher with stock wineserver.

The exact O2 test exited **1** on a free Xvfb display. The DLL/PE builds passed,
but self-host startup returned `c0000001` and `DESK result FAIL`, with
`NtCreateUserProcess attempts: 9 (wine-internal: 9, guest: 0)` including
wineboot and explorer. Wine 10.0 `dlls/ntdll/unix/env.c:build_initial_params`
calls `run_wineboot` before PE entry; a fresh stock server has no boot event,
so explorer starts before the probe can call the new export. DLL loading
succeeded, but the existing explorer desktop prevents thread ownership.
A bootstrap arrangement before PE entry remains necessary; no spawn metrics,
security gates, verifier defaults, or nofork/wx/noproc constraints were changed.
Successful initialization and repeated/concurrent caller behavior remain
unverified at runtime.

The remaining exact-chain checks were run separately: plain stock passed all
DESK checks; `make -C x64/uwp-host test` passed (172/0 shim, 7/0 verifier);
`run-acceptance.sh` passed. Build-only proof covers the DLL. The failed desktop
proof used our launcher and Wine's own PE loader with a stock wineserver,
without uwph. Existing hello acceptance passed THROUGH launcher -> Wine
loader -> shim -> uwph under nofork,noproc,wx; that does not prove the desktop
thread. Linux only; Xbox untested.

Full output and reason: `/workspace/x64-run/lanes/B34/B34-O2.result`.
Video: `/workspace/x64-run/videos/B34-O2.mp4` (14.67 seconds, free Xvfb and
ffmpeg, no Encom). No commit, push, or remote operation was performed.

## B35 O1

Added `wine-inproc-desktop.patch`: append `NtUserStartInprocDesktop` before
`NtUserCallCount`, with user32 and wow64 callback table entries. win32u reads
`WINE_INPROC_DESKTOP=1` exactly once with `pthread_once`; only that opt-in
selects the callback instead of the existing explorer process block. The
unset path retains the original process block unchanged. user32 loads
`uwpdesktop.dll` using Wine's loader and calls `uwp_desktop_start(10000)`,
returning errors without a fallback spawn. A per-thread guard prevents callback
recursion, the desktop thread returns `STATUS_POSSIBLE_DEADLOCK` rather than
waiting on itself or forcing a premature desktop, and concurrent initial
callers share the helper's atomic owner and readiness event. The ordinary
callback completion uses the existing force=1 desktop re-query semantics.

`prepare-wine-desk.sh` reproducibly applies both desktop patches and rebuilds
and stages user32.dll, win32u.dll and win32u.so together with uwpdesktop.dll.
The nofork probe and Megabonk scripts build this set, copy the private DLL to
the disposable prefix, and export the opt-in beside UWPH_INPROC_SERVER=1.
The probe now displays the actual uwpjail seccomp enforcement line.

Built the missing generated probe with `bash x64/pe/tests/build.sh`, then ran
the exact O1 command on a free Xvfb display (:4): exit 0, all mandatory DESK
checks PASS, `DESK result PASS`, `NtCreateUserProcess attempts: 0
(wine-internal: 0, guest: 0)`, `nofork=1`, and no failed-to-start-explorer
message. Evidence: `x64/uwp-host/harness/out/desk-probe/evidence-wd9KrMCF`.
This is THROUGH uwp_run.sh(nofork,wx,noproc) -> launcher -> Wine's own W^X
loader -> shim -> uwph, with in-process server and desktop, without
`--self-host`. Stock Wine was used only to package the prebuilt prefix, not
as the runtime proof. Linux proof only; Xbox untested. The passing run still
logs `err:menu:copy_sys_popup Unable to load default system menu`; systray
is informational and absent. O2 regression gates and Megabonk runtime were
not run under this O1-only order. Video:
`/workspace/x64-run/videos/B35-O1.mp4` (6.2 seconds, Xvfb + ffmpeg; no Encom).
No security constraints, verifier defaults, surface gates, or remotes were
changed; no commit or push was performed.

## B70 O1

2026-10-25, stock Wine 10.0 on the Linux box only (not THROUGH-uwph, not Xbox). Added our mingw-x86_64 WinHTTP directory/WSS diagnostic and a fresh-prefix runner. The runner also reruns the earlier readiness net_probe.exe in the SAME prefix when present; it deletes only its own temporary prefix on exit. No credentials, Valve binary changes, certificate exceptions, or O2 environment fixes.

Evidence `/workspace/x64-run/lanes/B70/o1/https_probe.log`: `winhttp_status=200 err=0`, `wss_status=101`, `wss_upgrade=1 err=0` against a websockets :443 endpoint selected from the live directory response. Both API and CM certificates pass `CertGetCertificateChain` (`chain_error_bits=0x00000000`) and SSL hostname/server policy (`error=0x00000000`). `adapters_status=0 bytes=2128`; DNS returns IPv4 `198.18.0.1`. `readiness-net.log`: `SUMMARY net_probe pass=7 fail=0`. `curl-control.log`: HTTP 200, TLS verify 0, remote IP 198.18.0.1. TLS verification remains enabled; requests use their original DNS hostnames (preserving Host/SNI), with NO_PROXY explicitly selected in our diagnostic.

Root-cause boundary: the claimed general “Wine HTTPS failure” does not reproduce in either WinHTTP probe. These results rule out a universally broken Wine crypt32 root store or fake-IP IPv4 route for these hosts in this fresh prefix. They do NOT rule out a Valve-specific trust store, TLS/ALPN configuration, mapped-IPv6 path, proxy handling, or prefix/bitness-dependent adapter failure. Prior unmodified steamcmd evidence, extracted without account identifiers to `prior-valve-failure.txt`, has `GetCMListForConnect -- web API call failed (status = 0)` and `CalcUnIPThisBox - GetAdaptersAddresses returned 13`; neither appears in our x64 diagnostic. Error 13 is ERROR_INVALID_DATA, but correlation is not proof it causes the HTTP failure. Exact Valve-stack root cause remains UNDETERMINED: O1 diagnosis is BLOCKED at that boundary, not an invented TLS fix. No O2 changes made. Exact test and video are recorded in `/workspace/x64-run/lanes/B70/exact-test.txt` and `/workspace/x64-run/videos/B70-O1.mp4`.
