# Linux passthrough harness (O1)

Run `make -C x64/uwp-host/harness backend-test` from the repository root.
This separately built archive selects `UWPH_BACKEND_LINUX_PASSTHROUGH`; default
Linux stub and Windows APP builds retain their existing behavior. It compiles
uwp-host's own sources and dispatches the selected public uwph APIs to the
harness adapters. No Wine loader source or syscall instruction bytes change.

Implemented slice: open/openat/creat, read/write/pread/pwrite/close/lseek,
stat/lstat/fstat/fstatat, mmap/mprotect/munmap, clock_gettime/clock_getres,
nanosleep, gettid and typed futex WAIT/WAKE (including PRIVATE_FLAG).
Other futex operations fail with ENOSYS. Other existing exports retain their
previous implementation; this is not a production Linux backend or a complete
libc. No mounts/path containment policy is asserted for Linux passthrough.

The backend owns a private 1024-slot descriptor table. Returned descriptors are
0x100000 + slot, outside the synthetic table's 0..4095 range. Native Linux fd
numbers never escape adapters; stdio and arbitrary borrowed fds are rejected.
The selected file APIs reject synthetic descriptors. Conversely synthetic-only
APIs such as dup/poll are not adapters for these handles. rwlock reader pins
cover active operations and exclusive close frees a slot (including EINTR);
blocking file I/O can delay close. As with POSIX fds, a closed slot can be reused.
Do not mix the two descriptor namespaces or use them as native Linux fds.

Host operations use inline x86-64 raw Linux syscalls and translate kernel errors
to thread-local uwph_errno. pthread rwlocks provide internal lifetime locking;
these are not routed host calls and must stay outside any later shim surface.
Linux memory/protection semantics are passed through, including Linux RWX
behavior; they do not validate UWP codeGeneration or Windows memory policy.

The 51 direct checks go THROUGH public uwph entry points and this selected
backend. Fixture directory creation/cleanup, pthread coordination, fork/wait
protection observation, mincore unmap observation and syscall gettid comparison
are host-only. The waiter publishes readiness atomically; a positive wake count
confirms kernel enrollment. No stock Wine, our launcher, ntdll.so, Windows
backend or Xbox-device integration is exercised by this test.

`harness/record.sh O1 -- make -C x64/uwp-host/harness backend-test` starts a
private Xvfb using -displayfd, runs the command in xterm and captures with
ffmpeg x11grab to `/workspace/x64-run/videos/R4H-O1.mp4`. Dependencies:
Xvfb, xdpyinfo, xterm, ffmpeg, ffprobe. It bounds the command to 50 seconds
(then a 2 second kill grace) and video to 58 seconds, preserves command status,
and cleans up only PIDs it started. Commands must not daemonize. A timed-out
command returns 124; completed commands return their status. Logs and status
are saved under `/workspace/x64-run/v2/R4H/`. Video checks are host-only proof
of execution on this Linux box, not Xbox Dev Mode validation.

## Bounded ELF shim (O2)

Run `make -C x64/uwp-host/harness shim-test`. The target uses LD_PRELOAD only
for its two owned, dynamically linked ELF probe processes. Its version script
exported exactly open/read/close/mmap/munmap, _exit, and the explicit
uwph_shim_report instrumentation function (R4S O2 below adds the traced set).
There is no generic syscall wrapper, process-wide trapping, or Wine loader change.
The five SysV prototypes are explicitly adapted to the existing uwph APIs;
compile-time checks cover the selected scalar/flag ABI. No struct-bearing libc
symbols are exported. open consumes a mode only for O_CREAT or full O_TMPFILE.
Unexported symbols (including open64/openat/fstat/write and libc hidden calls)
remain outside this proof.

Calls using private backend descriptors route through the real public uwph
entry points. Anonymous mmap and munmap route too. Borrowed native descriptors
in read/close/file mmap are explicit raw-Linux bypasses counted per operation;
they are never adopted into uwph. Other APIs cannot use the private descriptors.
This intentionally bounded shim is not a general replacement libc and must not
yet be used as evidence of Wine integration. Linux raw memory semantics still
do not demonstrate UWP memory policy.

Instrumentation is separately linked into the shim; optional weak hooks in the
selected backend entries (five; mprotect is the sixth since R4X O2) increment
atomic totals independently of wrapper counts. A mutex protects
immediate-caller module names and per-module wrapper and backend counts; TLS carries attribution only for the current wrapper call.
dladdr identifies the immediate return-address module (not a complete causal
stack). Module names are hex-encoded in reports, alongside process PID.
Thirty-two module slots and 511-byte names are bounded; overflow and unknown
origins cannot pass the probe verifier. Calls entering the backend directly
without wrapper context are counted as unattributed. No caller is assumed to be
Wine: future acceptance must match the real ntdll.so identity separately.
Recursion fails closed with EDEADLK and increments its own counter, rather than
calling a resolver or reentering the backend. Counters describe only this selected
surface, not hidden libc calls, inline syscalls or all host operations.

uwph_shim_report explicitly snapshots counters, preserves errno, and uses only
raw openat/write/close for report I/O (snprintf formats memory; it is not an
interposed export). That instrumentation I/O is separately classified and is
not routing evidence. UWPH_REPORT is a trusted per-process harness output path;
later multiprocess tests must choose unique paths. _exit snapshots before raw
exit_group; a destructor is supplementary. Reports are coherent for the joined,
quiescent probe; asynchronous snapshots during live operations can include
in-flight wrapper calls. Fork, signal-handler instrumentation and arbitrary
process teardown are not supported by this bounded harness.

The test runs four threads, each performing 50 fixture open/read/file-map/close
and anonymous-map cycles, checking real contents, writable memory, errno
preservation and successful unmaps. It also checks O_CREAT mode normalization,
ENOENT, EBADF and EINVAL, and deliberate native-fd bypasses. Expected backend
entries per positive process: open=202, read=200, close=201, mmap=401,
munmap=401; bypass read=2/close=2, all others zero. Verification matches exact
wrapper/backend totals and module attribution, with zero recursion, unattributed
entries and module overflow. One run explicitly snapshots and returns normally;
the other calls _exit without any earlier explicit snapshot, proving that reports
do not depend on destructors. A negative run without the shim exercises the same
operations through stock Linux libc and must produce no uwph report.

| O2 evidence | Routing and limit |
| --- | --- |
| Two positive ELF runs | THROUGH shim -> public uwph -> O1 Linux backend for the selected operations; declared bypasses remain native Linux |
| Negative ELF control | Stock Linux libc; no uwph proof report |
| Counters/report writer | Instrumentation only, not proof of every host operation |
| Build/ABI/video checks | Host-only evidence on this Linux box |
| Our launcher / stock Wine | Neither used; neither ELF run is Wine or Xbox-device proof |

Capture the acceptance command with
`x64/uwp-host/harness/record.sh O2 -- make -C x64/uwp-host/harness shim-test`.

## Real Wine artifact inventory (O3)

`./prepare-wine.sh` selects only the installed Debian Wine 10.0 artifacts at
`/usr/lib/wine/wine64` and `/usr/lib/x86_64-linux-gnu/wine/`, verifies the local
`/tmp/wine-src/VERSION`, records canonical paths/SHA-256/package identity in
`out/wine/identity.json`, and hashes every supplied local source file into
`out/wine/source.sha256`. Installed build configure arguments are unavailable;
that fact is recorded rather than inventing a configuration. No local source
build, host patch, network fetch, loader change, or prefix preparation occurs.
Matching installed symbols bind successfully, so a source build is unnecessary.
The installed binary checksums, not an assumed correspondence to the supplied
source tree, identify the tested Wine build.

`make wine-surface-check` regenerates `out/wine/imports.tsv`, raw ELF symbols,
relocations, dependencies and disassembly, plus `direct-syscalls.tsv`. The
committed `surface.tsv` explicitly classifies every undefined dynamic symbol,
including weak/runtime and non-libc dependencies. A changed import list fails
closed until reviewed; absent shim wrappers are unsupported, never inferred
from the existence of an unrelated uwph API. `check_surface.sh [policy.tsv]`
checks exact inventory membership, selected shim exports and actual dynamic
linker binding diagnostics. Parent `make all` now invokes this bounded check
instead of the missing `tools/check_surface.sh`/`tools/surface.tsv`. This changes
the documented gate: success means complete classification, not full support.

The generated owned ELF diagnostic dlopens the **real** ntdll.so with RTLD_NOW
and records glibc LD_DEBUG=bindings. It checks that all six selected versioned
imports actually resolve to the canonical shim path, rather than merely seeing
LD_PRELOAD. No Wine entry point or PE is invoked. Five imports are classified
`routed` for the selected shim surface (with O2 native-fd bypass limitations);
`_exit` is an explicit host fallback that flushes instrumentation and performs
raw Linux exit_group. The diagnostic is host-only and is **not execution proof**
THROUGH uwph. O4 must provide caller-attributed backend counters from PE runs.

Current inventory: 199 imports, comprising 5 routed, 33 libc internal, 1 explicit
host fallback and 160 unsupported. Two decoded inline syscall instruction sites
are saved by address; both remain unsupported direct sites. This is a static
instruction inventory, not dynamic syscall-number recovery or a reachability
claim. The libc `syscall` import is independently unsupported. Hidden/local
libc calls, calls inside retained libc helpers, later dlopen/dlsym additions,
version differences on other installations and inline syscalls evade the shim.
Versioned relocation binding works for this exact installed artifact only.
Unsupported entries and direct sites block any full Unix-side coverage claim.
Libc-internal classification is a retained Linux dependency, not UWP support.

| O3 evidence | Routing and limit |
| --- | --- |
| Inventory, SHA manifests, builds and ABI checks | Static/host-only; no PE execution |
| Real ntdll relocation diagnostic | Host dynamic linker binds six imports to shim; no Wine entry point execution or uwph backend proof |
| O1/O2 regression tests | THROUGH selected public uwph/backend; O2 negative control uses stock Linux libc |
| Default Linux smoke | Portable uwph logic and ENOSYS stub; not Linux passthrough or Windows backend |
| Stock Wine / our launcher | Only stock Wine `--version`; no PE run and no launcher run in O3 |
| O3 video | Linux-box Xvfb terminal capture; not Xbox device proof |

Record with `./record.sh O3 -- make -C x64/uwp-host/harness wine-surface-check`
from the repository root (use the full harness path for record.sh).

## O4: real PE execution (routing observed; wineboot-free gate BLOCKED)

Build owned PEs with `x64/pe/tests/build.sh`, then run
`make -C x64/uwp-host/harness wine-test`. `run-wine.sh` uses our unchanged
`run_via_wine.c` launcher and the O3 checksum-pinned Wine 10.0 loader/ntdll.
It copies `/workspace/megabonk/pfx-stock` (override with
`UWPH_FIXTURE_PREFIX`) into `out/wine-o4/prefix`; never uses the normal prefix.
The source is an already initialized stock-Wine fixture. `fixture.txt` records
its source and registry/timestamp checksums; copying is setup, not routing
proof. No preparation command runs wineboot or initializes a new prefix.
Delete only this harness-owned copy to select a different fixture on a later
run. A lock prevents concurrent tests sharing the owned prefix.

A separate **unshimmed Linux wineserver64** is explicitly started for this
prefix and stopped afterward. `WINESERVER=/bin/false` prevents an accidental
replacement inheriting preload. Routing this real server is deferred: this
order isolates ntdll callers and native Linux server behavior is not in-process
Xbox-server feasibility evidence. LD_PRELOAD is scoped to each owned launcher
and inherited Wine children, never the shell, timeout/strace, or server.

O1/O2 private descriptor numbers cannot be passed to unwrapped fstat/fcntl or
Wine's server descriptor transfers. O4 therefore builds a separate
`wine_linux_backend.o` with `UWPH_HARNESS_NATIVE_FDS`, linked before the archive
into `libuwph_wine_shim.so`. Public uwph open/read/close/mmap/munmap still execute
the same backend syscall adapters/counter hooks. This variant returns real
Linux descriptors, borrows them for read/map, and consumes them on close;
other libc/Wine calls may use them directly. It has no private-slot rwlock:
blocking Wine pipe reads must not prevent another thread from opening/closing
unrelated native descriptors. Only this mixed Linux harness uses this contract;
synthetic fd APIs must never consume these numbers. O1/O2 retain private slots
and their existing exact-count tests.

Every run has a unique evidence directory with both PE logs, exec traces,
statuses, identity/inventory copies and PID-suffixed reports. The `_exit` hook
flushes reports; PID suffixes prevent a Wine child overwriting main-process
proof. No signal handler, remote inspection, Wine loader change, or fake
Unix-side object is used. `verify_wine.py` matches the report PID to the traced
launcher, resolves caller paths to canonical ntdll.so, checks hashes and real
runtime relocation bindings, and requires nonzero matching wrapper/backend
counts for all selected operations (five in R4H O4; six since R4X O2 added
mprotect), zero selected bypasses, zero
recursion/unattributed/overflow, expected PE output, and Wine module-load traces.
Control runs use our launcher and stock Wine without preload and must have no
report. Each PE run has a 12-second timeout plus 2-second kill grace.

**Observed blocker:** Wine 10.0's `build_initial_params` in
`/tmp/wine-src/dlls/ntdll/unix/env.c` unconditionally calls `run_wineboot` for a
new top-level process. An initialized registry and `.update-timestamp` do not
skip this call. `WINEDLLOVERRIDES=wineboot.exe=d` stops the initialization PE
from loading, but Wine still creates a Linux child to attempt it (exit 53,
`failed to start wineboot c0000135`). The verifier rejects both attempts and
actual spawns, including this case. Both routed PEs nevertheless exit 0 and
show real ntdll -> shim -> uwph backend traffic, but the overall `wine-test`
returns failure and **does not print `Wine uwph verification: PASS`**. No warm
Wine fixture, synthetic boot event, loader edit, or suppressed trace is used
to turn this failure green. A later order must resolve the cold-start/no-boot
fixture requirement within the allowed process/loader constraints.

The selected routed surface is only open/read/close/mmap/munmap (R4H O4),
plus mprotect since R4X O2. Since R4X O2 the routed runs also use the private
W^X ntdll.so (`out/wine-wx`, see the R4X O2 section): the shim refuses
WRITE|EXEC and stock ntdll maps PE images RWX, so routed stock Wine now exits
13. Controls stay on stock Debian Wine without preload. The O3
inventory's other 160 unsupported imports, 33 retained libc/internal imports,
`syscall()` and two inline syscall sites remain Linux dependencies/bypasses.
Passing these counters would not mean full Unix-side portability. Wine's
native Linux TLS/signals and memory behavior are not UWP equivalents.

| O4 evidence | Routing and limit |
| --- | --- |
| Two routed owned PEs | Our launcher -> real Wine loader/ntdll.so -> shim -> public uwph -> Linux backend, five selected symbols (six, W^X ntdll.so since R4X O2); overall no-wineboot gate fails |
| Two unshimmed controls | Our launcher + stock Wine; no uwph report |
| Prefix copy, server and inventory | Stock-Wine setup / separate Linux stand-in dependencies; not uwph proof |
| O1/O2 regressions | THROUGH uwph Linux backend; O2 negative control is stock Linux libc |
| Linux smoke / ABI / archives | Portable uwph with default stub / host-only checks, not Wine routing |
| Xvfb/xterm/ffmpeg recording | Linux-box execution evidence of the failed gate, not Xbox-device validation |

Capture with `x64/uwp-host/harness/record.sh O4 -- make -C
x64/uwp-host/harness wine-test` (on one shell line). Its nonzero status records
the rejected acceptance run honestly; the resulting MP4 is still valid footage.

## O5: cooperative Windows futex/signal regression

`make -C x64/uwp-host/harness windows-futex-test` builds a separate desktop
artifact under `out/windows-desktop/` and runs the owned PE with stock Wine.
It exercises **THROUGH uwph Windows thread/futex/signal code**; stock Wine
loads the PE and provides Win32 waits/threads. It does not route Wine's Unix
ntdll.so through the Windows backend. `wine-test` remains the independent O4
Linux routing check, including its existing wineboot-free acceptance blocker.
The initialized owned O4 fixture is reused under its lock; no normal prefix,
Wine loader changes, or initialization commands are used.

Twenty repetitions cover signals queued before wait and during a published
wait, masked signals at both points with delivery on unmask, and ordinary wake
while the word remains equal. The per-waiter sequence/registry lock handshake
closes publication-to-sleep races. Signal kicks increment the sequence without
claiming a normal wake; FUTEX_WAKE claims at most its requested waiter count.
Zero wake is checked. Tests save return/errno immediately, use a handler that
clobbers errno, check exactly one delivery, and check ESRCH after thread exit.
Waiters have a two-second bound and publication handshakes have deadlines.

The futex-only run skips initialization, exception registration and live TEB
mutation. Default UWP signal initialization enables cooperative delivery only;
exception registration is compiled solely with `UWPH_DESKTOP_SIGNAL_TEST`.
Live guest-TEB exception dispatch is disabled. Synthetic context/fake-array
checks remain synthetic. Full desktop smoke is explicitly test-only and uses
that macro for its existing desktop exception tests. The default smoke skips
live exception execution with an explicit message.

`make -C x64/uwp-host/harness windows-smoke-test` runs the entire desktop suite
and preserves its exit status. O6 resolves the fixture path for the CreateFile2
checks (see below); the futex-only target is not a full-suite pass. Linux backend/stub tests do not validate this Windows
fix. Record O5 locally with `record.sh O5 -- make -C
x64/uwp-host/harness windows-futex-test`; the video shows this Linux box, not an
Xbox device. No Encom is used.


## O6: absolute Windows file fixture

`make -C x64/uwp-host/harness windows-file-test` runs the focused file checks;
`windows-smoke-test` runs the full desktop suite including O5 signals. Both
use the owned O4 prefix under its lock. The runner resolves the repository's
`tests` directory with `realpath`, converts it with `winepath -w` using that
prefix, validates an absolute drive path, and passes the quoted Windows root
to the PE. This conversion is fixture setup only. The previous relative root
was interpreted from the harness CWD and pointed to a nonexistent nested tree.
No fs.c change, API fallback, writable filesystem expansion or Wine loader
change is needed.

The PE checks initialization and stops if it fails. It logs process CWD,
virtual CWD, translated path and mount result, then performs a separately
labelled stock Win32 GetFileAttributesA path control. Immediately after
uwph_open it saves GetLastError and uwph_errno before printing. Read/content,
regular-file size, lseek/read and close assertions exercise Windows uwph's
CreateFile2 backend. A failed open remains a failing assertion and explicitly
lists the dependent assertions skipped. Successful runs skip none of these.

Coverage: file/signal tests go THROUGH Windows uwph hosted by stock Wine's
Win32 implementation. GetFileAttributesA and winepath are stock Wine controls
or setup, not routing evidence. These runs do not use run_via_wine and do not
route Wine's Unix ntdll.so through uwph. The separate `wine-test` exercises our
launcher and selected real ntdll.so -> shim -> Linux uwph calls; its existing
wineboot-attempt acceptance blocker remains. Local `record.sh O6 -- make -C
x64/uwp-host/harness windows-smoke-test` captures Linux Xvfb/xterm via ffmpeg,
not Xbox device validation. No Encom is used.

## O7 final evidence ledger (acceptance BLOCKED)

From the repository root run the four O7 commands in ORDERS-R4H.md.
`make evidence-replay` runs a bounded live backend positive test, then displays
labelled saved O7 real-Wine regression and O6 Windows smoke results with paths.
It pauses for readability within the recorder's limit. Replay exit 0 means the
display completed, not that Wine acceptance passed. All seven recordings exist. O5 was recovered with an explicitly labelled O7
replay because its original terminal command header was damaged; original
video/log/status are retained in v2/R4H/O7-original-O5/. O4's recording correctly has exit 2.

`verify-evidence.sh` is host-only and fails closed: ffprobe checks every video
stream/duration, recorded command and exit sidecars must match the terminal log,
each result needs a coverage table, and SHA-256 manifests seal the recordings.
It reruns the real Wine identity/binding/caller/counter/negative-control verifier
on the saved O7 run, including rejection of wineboot attempts. Saved ELF reports
are independently verified too. It currently returns 1, printing
`evidence verification: BLOCKED`; it must not print PASS while O4 or the O7
regression fails. The original recordings show Linux-box execution only.

Final ledger: `/workspace/x64-run/RESULT-R4H-O7.md`; per-order results O1–O6
retain their original scope and failures. The five routed real ntdll operations
are open/read/close/mmap/munmap. Windows uwph file and futex smoke hosted by
stock Wine is separate evidence. Linux libc, raw/inline syscalls, Linux TLS and
signals, initialized-prefix setup and separate unrouted wineserver remain
stand-in dependencies; there is no full UWP or Xbox acceptance claim. No
DONE-R4H is written because the required no-wineboot gate remains unresolved.

## R4S O2: traced set (O1 top 20)

`shim.map` now also exports the 20 most-hit unsupported imports from
`surface-trace.tsv`: snprintf, fopen, __isoc23_fscanf, fclose,
pthread_getspecific, pthread_sigmask, feof, stat, write, pthread_mutex_lock,
pthread_mutex_unlock, writev, getenv, asprintf, sigaddset, fcntl, setlocale,
time, getuid, realpath. Each shim entry calls its public uwph entry, which bumps
a per-symbol atomic counter (`uwph/trace.h`; no logging on the counted path).
`check_surface.sh` takes the routed set from `shim.map` and requires the trace
counter names to match it; surface.tsv now has routed=25, unsupported=140.

Linux passthrough backend: stat/write/writev/fcntl/time use the raw-syscall
adapters. Native FILE, pthread key, pthread_mutex_t, sigset, locale, env,
getuid and realpath semantics stay with host libc, resolved once with
`dlsym(RTLD_NEXT)` (missing symbol aborts; nothing is emulated silently). This
keeps the native ABI for companion calls that are still not interposed (fgets,
setenv in Wine, ...). The passthrough `uwph_setenv/putenv/unsetenv` use the same
native table as `uwph_getenv`. In private-slot mode write/writev/fcntl accept only
owned descriptors and fcntl F_DUPFD* is refused (a dup would leak a native fd).
Only the separately built Wine shim (native-fd mode) accepts arbitrary fds.

Win32/UWP backend: same public entries over existing uwph objects: CRT
vsnprintf and setlocale (UWPH_LC_* Linux numbering mapped to CRT constants; no
CRT LC_MESSAGES, so only C/POSIX is accepted for it), uwph FILE over
CreateFile2-backed fds with a bounded integer/%s/%c/%n scanner (floating
point and %[ fail with ENOSYS), uwph env table, keys, mutexes, signal masks,
fixed uid 1000, path-normalizing realpath (no symlinks).

`tests/traced.c` covers every symbol on all three builds (Linux stub, Win32
under wine64, Linux passthrough via `make -C harness traced-test`). It checks
behavior and that every counter moved. All three run from `make test`. The
shim report now has `trace <symbol> <count>` rows. `verify_wine.py`
requires every traced counter to be nonzero in routed hello/hello_k32 runs.
Those counts are process-wide uwph entries, not ntdll-attributed like the five
O2 selected families. The overall wine-test gate stays BLOCKED by the existing
wineboot attempt, as it is in the stock control runs.

## R4S O4 acceptance

`make acceptance-test` runs both owned hellos through this harness's launcher,
Wine's own loader, and the native-fd uwph shim under
`/workspace/xbox-readiness/tests/uwp_run.sh nofork,noproc,wx`. The default includes
wx because the installed jail supports it. An initialized explicit fixture is
required, as in O3. A separate stock Linux wineserver remains outside the jail.
No Xbox device acceptance is implied.

The original strict noproc verifier still requires positive ntdll counts for
open/read/close/mmap/munmap with zero bypass/recursion/unattributed/overflow.
The added verifier requires all capped O1 priorities 1–20 to have positive uwph
entry counters and real ntdll relocation bindings to the shim. These trace
counters are process-wide, not per-caller counts; binding evidence establishes
ntdll routing. Remaining O1 imports and direct syscalls remain excluded.

O4 outcome: BLOCKED. Available wx seccomp causes hello_k32 exit 13 with
`wine: failed to load start.exe: c000000d`. `UWPH_CONSTRAINTS=nofork,noproc
./run-acceptance.sh` passes both hellos and all 20 traced entries as a diagnostic;
that override does not satisfy the default wx gate. Full `make -C
x64/uwp-host test` passes (Windows unit tests run under stock Wine, separately
from the routed Linux guest proof). External status surface/noproc rows are in
`/workspace/xbox-readiness/STATUS.md`; logs in
`/workspace/x64-run/lanes/R4S/O4-*.log`; free-Xvfb/ffmpeg video in
`/workspace/x64-run/videos/R4S-O4.mp4` (4.6 seconds).
## R4X O2: uwph_protect and W^X PE image mapping

`uwph_protect(addr, len, prot)` (include/uwph/mem.h) is the strict W^X
transition: it refuses WRITE|EXEC with EACCES and has no RWX tracking or
fallback. Linux passthrough = mprotect; Win32/UWP = VirtualProtectFromApp
only (ENOSYS if it cannot be resolved; executable protection needs the
`codeGeneration` capability in a real package). The legacy `uwph_mprotect`
RWX emulation is unchanged for other callers.

The harness shim now also exports `mprotect` (routed to uwph_protect; counted
as `mprotect`), and its `mmap` refuses WRITE|EXEC before any backend entry, so
a refused RWX request shows as a wrapper call without a backend entry
(mmap), or as a wrapper call whose uwph_protect returned EACCES before the
backend (mprotect). This applies to every shim user, so `wine-test`'s routed
mode now runs the W^X ntdll.so below; with stock ntdll it would exit 13.

`prepare-wine-wx.sh` builds a private `out/wine-wx/` tree: upstream Wine 10.0
(`/tmp/wine-src`) + the Debian 10.0~repack-6 Unix-ntdll patches (install paths,
server tmpdir, two virtual.c fixes, regenerated protocol version 1) +
`wine-image-wx.patch`. R4X rebuilt only ntdll.so; B18 also rebuilds PE ntdll.dll
for missing-import stubs (see below). wine64 is copied; other DLLs link to the
installed Debian Wine. The patch keeps Wine's own
loader algorithm and changes only the protection order: image pages are mapped
RW (copy-on-write), sections are read and relocated, then each section gets its
final R/RW/RX via mprotect. The view keeps EXEC as its allowed maximum
(AllocationProtect PAGE_EXECUTE_WRITECOPY; later NtProtectVirtualMemory to
PAGE_EXECUTE_READ works). Flat (non page-aligned) images and writable+executable
sections are refused with STATUS_NOT_SUPPORTED; there is no RWX fallback.

Exact test, from the repository root:
`make -C x64/uwp-host && /workspace/xbox-readiness/bin/uwpjail --wx -- x64/uwp-host/harness/out/run_hello`.
`run_hello` runs hello.exe through our launcher -> W^X Wine loader/ntdll.so ->
shim -> uwph Linux backend. It exits 3 when hello passes but no protect call
reached the backend or an RWX mmap/mprotect was refused. `WINE_LOADER=/usr/lib/wine/wine64`
selects the stock Debian ntdll as a control; under `--wx` that exits 13.
`make -C x64/uwp-host/harness protect-test` checks the API alone under --wx.

| R4X O2 evidence | Routing and limit |
| --- | --- |
| run_hello under uwpjail --wx | THROUGH our launcher -> Wine's own loader (patched private ntdll.so) -> shim -> uwph; Linux seccomp W^X, not UWP/Xbox |
| Stock-ntdll control | Our launcher + stock Debian Wine, same shim; fails under --wx |
| protect-test | uwph API only, no PE |
| Windows uwph_protect | Build-only (mingw APP headers); no VirtualProtectFromApp run |
| Separate wineserver | Stock Linux wineserver64 (unshimmed), as in R4H O4 |
| wine-test (R4H O4 gate) | Routed = W^X ntdll.so + shim -> uwph, both PEs exit 0; controls = stock Wine; still BLOCKED only by wineboot attempts |

## B18: remaining W^X inventory and missing-import stubs

`prepare-wine-wx.sh` now builds both Unix ntdll.so and PE ntdll.dll. The
additional `wine-loader-stub-wx.patch` changes Wine's `allocate_stub`: allocate
one RW region for each missing-import stub, fill it, protect it RX, flush the
instruction cache, then publish its address. A published stub's page is never
reopened for writes when a later import needs a stub. The original stub-count
limit remains; failure returns Wine's existing failure sentinel. The allocation
cost increases to at least one page per stub (only for unresolved imports).
Other PE DLLs remain symlinks to the installed Debian Wine distribution.

The shim logs `UWPH_RWX pid=... op=... caller=<module>+<offset>` immediately for
RWX requests, including those rejected before any syscall. Pair this with
`strace -f -e trace=mmap,mprotect`; `-k --status=failed` attributes denied native
syscalls without unwinding every successful mapping. Do not infer zero requests
from zero syscall hits alone. Neither diagnostic changes the refusal policy.

Run the focused missing-import probe with
`make -C x64/uwp-host/harness stub-test`. This uses the same initialized isolated
prefix, explicit stock Linux wineserver, private `WINE_LOADER`, shim `LD_PRELOAD`,
launcher `out/run_via_wine`, and `uwpjail --wx` used for hello.
It imports two nonexistent `version.dll` exports and checks that both generated
addresses have PAGE_EXECUTE_READ protection and separate allocation bases. It
never invokes the missing functions. See NOTES-x64.md `## B18` for recorded
commands, counts, console reachability, and limitations.
