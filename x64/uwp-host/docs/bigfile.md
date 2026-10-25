# Opt-in split-file prototype (B16 O1)

Console behavior is **unconfirmed until n7 runs on the target Xbox**. Linux
success does not establish a console limit, API availability, or game support.

## Public evidence and scope

Microsoft's [Xbox UWP documentation](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/)
provides the supported Dev Mode starting point. Its public
[FAQ](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/frequently-asked-questions)
does not establish a per-file read/write boundary for each directory.
The [file access documentation](https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions)
distinguishes the read-only package install directory from writable application
data, accessed through ApplicationData.LocalFolder. It does not specify an Xbox
2 GB per-file restriction. LocalState is app data, not the install directory.

Community reports narrow the *reported* problem, but do not prove its
implementation or applicability to current Series consoles. The closest
first-hand report points at a **per-app storage quota, not a per-file cap**:

* [libretro forum, post #7 (2020-12-19)](https://forums.libretro.com/t/uploading-multiple-files-one-xbox-one-with-retroarch/22133)
  (read in full for this doc): "the partition which the apps userdata files are
  stored in is limited to 2GB of storage no matter how big you set the storage
  size to in the dev environment". ROMs copied to RetroArch's `LocalState/Roms`
  hit it; pointing RetroArch at `D:\DevelopmentFiles\...` used the full dev
  partition instead. Xbox One era, writes via copy; read scope not stated.
* [RetroArch Dev/Retail storage discussion](https://www.reddit.com/r/RetroArch/comments/ldi2vq/),
  [GBAtemp RetroArch thread p.56](https://gbatemp.net/threads/official-xbox-one-retroarch-thread.518329/page-56)
  and [p.84](https://gbatemp.net/threads/official-xbox-one-retroarch-thread.518329/page-84)
  discuss file-size limits and clearing large content out of
  `LocalAppData\Retroarch\LocalState`. These pages block automated fetching;
  only search-result excerpts were seen, so they are cited as leads, not proof.
* Press summaries of the same era ("Dev Mode is apparently locked to files of up
  to 2GB", [TweakTown](https://www.tweaktown.com/news/90988/microsoft-cracks-down-on-emulators-xbox-series-removes-retroarch-app/))
  repeat the claim without a controlled test.
* [xllama uwp-constraints.md](https://github.com/gianlucamazza/xllama/blob/main/docs/uwp-constraints.md)
  lists "community reports a ~2 GB per-file limit in Dev Mode" as a caveat *to
  verify*; it gives no location, read/write scope or measurement.

Scope as best established: **LocalState (app data), write/copy path**, Xbox One
Dev Mode; **not** the install directory (no report of a read cap on packaged
content) and not `DevelopmentFiles`. Whether it is per-file or a total quota,
decimal 2 GB versus binary 2 GiB, and whether reads of an existing >2 GB file
fail are all unsettled. This matters: **if it is a total LocalState quota,
splitting files cannot help** and the fix is choosing a different documented,
already-granted storage location. The split layer only helps a true per-file
cap; n7 (below) is what distinguishes the two.

The public [GDK local-storage page](https://learn.microsoft.com/en-us/xbox/gdk/docs/features/console/storage/local-storage)
lists "2 GB of dedicated storage" for GDK temporary storage (T:) and an XR-133
1 GiB/5 min write-rate rule for Series consoles. That is a different SDK/storage
contract (GDK, not UWP Dev Mode) and establishes no UWP per-file limit, though
it shows 2 GB volume-level allocations exist on the platform. No install-folder
permission change, restricted capability or alternate security context is
proposed here.

## Format and behavior

`UWPH_BIGFILE_SPLIT=1` selects the **private-descriptor Linux harness backend**.
Unset, `0`, or any other value preserves existing behavior, including the old
ftruncate ENOSYS stub for ordinary files. The native-fd Wine harness deliberately
does not enable this prototype; split-file game execution is not yet supported.
Win32 compiles with APP-family headers and retains the explicit ftruncate ENOSYS
stub; no Win32 split implementation or console validation is claimed.

New files are represented from creation as `<name>.uwphpart0`, `.uwphpart1`, etc.
Using the same format for small files avoids an unsafe live migration when they
later grow beyond 2 GB. Existing ordinary files continue through the original
backend; there is **no automatic import/conversion**. Callers importing content
must create a new logical destination and copy through uwph positioned writes.
Suffixes are reserved in the prototype's dedicated data directory; do not mix
unrelated files or ordinary files with the same logical names there.

Parts contain raw bytes, each at most **1 GiB**, safely below either interpretation
of 2 GB. The contiguous part sequence terminates in a short part. Exact multiples
have a zero-length final sentinel. Logical size is full-part count times 1 GiB
plus the last part's size. No sidecar manifest is needed. Missing intermediate
parts are corruption and return an error. Limits are 128 open split handles,
1024 parts, and logical lengths strictly below 1 TiB.

`pread` and `pwrite` divide requests at boundaries; sparse ftruncate grows each
part without allocating its contents. Growth fills holes with zeros; shrink
truncates the terminal part and removes subsequent parts. `fstat` reports logical
size (other metadata, including allocated blocks, comes from part0). Sequential
read/write/lseek use the anchor descriptor's shared offset; positioned calls do
not move it. Reopening discovers the existing parts even for read-only access.
Relative paths pin the original directory descriptor. The private namespace lock
pins operations against close and serializes resize/positioned writes.

Read-only mmap assembles the requested range in anonymous RW memory, then changes
it to R. PRIVATE and SHARED requests both yield a **snapshot**, not a coherent
shared file mapping. It accepts aligned offsets and fully in-range lengths;
write/execute/fixed mappings are rejected with ENOSYS. Large views allocate their
full length, so production needs bounded windows and explicit lifetime policy.
This is data mapping only; it does not implement executable image loading.

## Prototype boundaries / production design

Path stat/lstat/fstatat, directory enumeration, rename/unlink, stdio bridging,
dup/fcntl/writev, append and writable mappings are not virtualized for split
files. Unsupported descriptor operations fail ENOSYS rather than reading part0
as the entire file. No crash atomicity, fsync guarantees, interprocess locks,
symlink hardening, part-name collision protection, rollback on partial resize,
or coordination between unrelated processes is provided. Files must remain in a
private, trusted directory; corruption recovery is manual. An interrupted shrink
can leave stale tail parts; a failed growth can leave a partially enlarged file.

A production layer must virtualize those paths and descriptor aliases, preserve
permissions for all parts, publish a versioned size manifest atomically, journal
resizes/imports, and reject conflicting part sets. The Windows implementation
should use documented UWP file APIs in the app's granted directories and mirror
the same logical operations. It must never extend filesystem permissions. First
measure whether segmentation helps: a *total LocalState quota* cannot be fixed
by splitting files. Preserve packaged content as read-only.

## Evidence and on-device follow-up

`make -C x64/uwp-host test` includes `harness bigfile-test`, which executes
`tests/test_bigfile.c` through public uwph entry points linked to the selected
Linux backend. It creates a **3 GiB sparse logical file**, checks physical part
sizes and sparse allocation, writes/reads across the 2 GiB boundary, tests holes,
EOF/reopen, negative arguments, access permissions, shrink/regrowth, sequential
offsets and read-only mmap. This is **THROUGH-uwph**, with no stock Wine involved
in the bigfile test. Existing Windows smoke tests in the exact suite run under
stock desktop Wine and are separate evidence. The Win32 stub is build-only for
this feature. No console behavior is inferred from either.

The existing kit has [n7 in rungs.c](../../on-device/rungs.c) and
[ladder.py](../../on-device/ladder.py), selected with `--big-file`. It requires
Xbox UWP activation plus verified ApplicationData LocalState, writes and rereads
2.25 GiB with CreateFile2/WriteFile/ReadFile, reports write/read errors, size and
verified bytes, then deletes the file. It skips if not activated or space is
insufficient. Run n7 before claiming the reported cap is reproduced or absent.
It currently tests unsplit LocalState only, not installed content or this layer.
Reading its outcome against the hypotheses above: a SKIP reporting ~2 GB or
less free in LocalState (its GetDiskFreeSpaceExW pre-check) points to a **total
quota**, which splitting cannot fix; a write error near 2048 MB with ample free
space points to a **per-file cap**, which this layer targets; PASS means no cap
in LocalState on that console/OS build. A follow-up rung writing two 1.25 GiB
files would separate the two cases directly.
A subsequent console experiment must separately test prepackaged >2 GB reads
from InstalledLocation, LocalState writes and reopen/reads, near-boundary sizes,
free-space/quota errors, and the split implementation after its Win32 port.
No n7 execution or package deployment was performed for B16 O1.
