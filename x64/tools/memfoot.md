Run `bash x64/tools/memfoot.sh && column -t x64/tools/memfoot.tsv`.

This Linux-only measurement uses the existing harness shim, our launcher and
Wine's private W^X loader. It copies the initialized fixture into its own prefix
and counts both client and separate stock wineserver sessions, including helpers
that retain those sessions after reparenting. Unrelated host services, Xvfb,
recorder and sampler are excluded. Steam is included when B10's trace artifact
is present; set MEMFOOT_STEAM_EXE to the installed real client in that case.
No Steam authentication or tokens are read.

Peak RSS comes from summed /proc/PID/smaps_rollup Rss at each 1 Hz tick.
Linux smaps_rollup does not expose Windows commit. The commit estimate sums
Size for smaps mappings carrying VmFlags ac (Linux accountable allocation).
It includes reservations charged under Linux accounting and is not a Windows
or UWP commit measurement. Shared RSS may be counted repeatedly. MiB (1024²
bytes) is labelled mb to match the requested output. peak_mb is the larger of
the two aggregate peaks, compared with 1024 and 5120 MiB. Peaks are sampled
lower bounds; short-lived hello processes and helpers can be missed between
ticks. Neither summed per-process maxima nor VmSize is used for budget totals.

Raw samples, per-stage logs, uwph counters and the top three individual process
RSS peaks are in x64/uwp-host/harness/out/memfoot/. Errors opening smaps on
process exit/zombies are counted. A timeout is a bounded observation, not proof
of gameplay or successful Steam startup. Early failures cannot establish a
stage's real memory budget. No optimization or on-device acceptance is claimed.
