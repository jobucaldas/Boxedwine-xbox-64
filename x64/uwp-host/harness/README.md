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
exports exactly open/read/close/mmap/munmap, _exit, and the explicit
uwph_shim_report instrumentation function. There is no generic syscall wrapper,
RTLD_NEXT resolver, constructor, process-wide trapping, or Wine loader change.
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
five selected backend entries increment atomic totals independently of wrapper
counts. A mutex protects immediate-caller module names and per-module wrapper
and backend counts; TLS carries attribution only for the current wrapper call.
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
