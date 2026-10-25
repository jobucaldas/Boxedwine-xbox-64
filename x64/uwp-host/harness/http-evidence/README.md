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

Observed: stock Wine and THROUGH launcher → Wine own WoW64 loader → B30 uwph
shim both returned HTTP 200, with 8,504 body bytes. This meets O1's allowed
success result; it does not establish the cause of B22's prior timeout.
The probe uses synchronous WinINet and PRECONFIG proxy selection. B22's trace
connected to 198.18.0.1:80; this fixture reports no registry proxy. Session flags,
proxy settings and asynchronous behavior are not claimed to match B22 exactly.
