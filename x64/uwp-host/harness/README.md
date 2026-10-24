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
