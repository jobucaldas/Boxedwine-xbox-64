# B30 O1

`http_probe.c` uses WinINet for an anonymous HTTPS GET of the exact manifest
URL in B22's `o2/run.0L0qbguI/bootstrap_log.txt`. The accompanying client trace
identifies WinINet through `err:wininet:open_http_connection` and error 12029;
loading both winhttp.dll and wininet.dll alone would not identify the transport.

Run `bash x64/uwp-host/harness/http-evidence/repro.sh` from the repository.
Build the current worktree shim first with
`make -C x64/uwp-host/harness out/libuwph_wine_shim.so`.
`B30_EVIDENCE` must select a fresh directory (default lanes/B30/o1).
`B30_WINE_ASSETS` selects an existing W^X Wine bundle, default B31's harness
output. The launcher and shim are built from B30; that existing loader bundle
is read only. The two runs use separate copies of the same stock fixture prefix,
not B22's Steam prefix, which is no longer present at its logged location.
No Steam execution, authentication, certificate overrides or transport fixes.

Evidence includes verbatim source excerpts, full WinINet/Winsock traces,
concise probe outputs, exit codes, launcher exec trace and uwph counters.
Video uses a free Xvfb display and ffmpeg, capped at 55 seconds.

The comparison returns HTTP 200 under stock Wine and under launcher → Wine's
own WoW64 loader → B30 shim, with completed body reads and exit 0. This is
Linux desktop harness evidence only. O1 transport acceptance remains BLOCKED:
the external, uninstrumented stock wineserver creates/connects the HTTPS socket.
It passes the TCP descriptor to the client through SCM_RIGHTS; the client uses
native socket syscalls on that descriptor. The client's shim connect counter is
zero. Selected shim operations do not prove end-to-end HTTP routing through
uwph, in-process operation, or Xbox/UWP acceptance. No production transport
fix is included.

`*-server.strace` identifies the server PID and its port-443 connect;
`uwph.strace` includes decoded TCP descriptors, descriptor transfers and native
client traffic. `routing-evidence.txt` extracts these records with the matching
client PID's shim counters. Probe stdout is separate from Wine debug stderr to
avoid interleaving body/completion lines. `validate.py` requires stock exit 0,
HTTP 200, completed body and final completion record. The uwph branch requires
the same success conditions or a Windows API FAIL record with exit 1; crashes,
timeouts and HTTP 200 followed by read failure cannot pass as success. It also
checks launcher/shim evidence and, on success, server connect, descriptor
transfer and native client traffic. A passing comparison gate is explicitly
not transport acceptance.

The probe uses synchronous WinINet and PRECONFIG proxy selection. B22's trace
connected to 198.18.0.1:80; this fixture reports no registry proxy. Session flags,
proxy settings and asynchronous behavior are not claimed to match B22 exactly.
This comparison does not establish the cause of B22's prior timeout.

## B30 O2 follow-up

O2 remains blocked. The fresh comparison in
`/workspace/x64-run/lanes/B30/o2/probe/` again completes with HTTP 200 in both
modes, but `routing-evidence.txt` again attributes the HTTPS connect to the
external stock wineserver, with native client traffic on the transferred fd.
No failing WinINet call was isolated that justifies a targeted uwph fix.
The non-passthrough `uwph_socket(AF_INET/AF_INET6, ...)` currently returns
`UWPH_ENOSYS`; this source limitation is not a demonstrated cause of B22's
WinINet error 12029 and cannot be represented as a repaired transport.

The B22 O2 `repro.sh` rerun exits 1 at its existing prefix prerequisite:
`test -f /workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64-uwph/system.reg`.
That prefix and Steam.exe are unavailable in this workspace, so no new Steam
manifest/download result is claimed. `B30-O2.mp4` records the HTTP comparison
on a free Xvfb display (55 seconds), not a Steam execution. Exact tests pass
after preparing the dedicated desktop smoke fixture from the existing stock
fixture: 172 unit checks and 500 Windows desktop smoke checks. The latter use
stock Wine to host uwph Windows code, not the Unix shim transport path.
