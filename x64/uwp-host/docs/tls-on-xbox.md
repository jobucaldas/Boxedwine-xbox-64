# Wine 10 TLS provider inventory (B44 O1)

Linux stand-in evidence only; Xbox console execution is SKIP. O1 inventories and
reproduces provider absence; it does not implement a replacement TLS provider.

## Complete source inventory

Searched the complete local Wine 10.0 source at
`x64/uwp-host/harness/out/wine-wx-source` for case-insensitive `libgnutls`,
`libcrypto`, and `SONAME_LIB(GNUTLS|CRYPTO)` (including configure definitions).
The only Unix library provider dlopen sites are:

| Unix library | Source call | Provider | Purpose |
| --- | --- | --- | --- |
| bcrypt.so | dlls/bcrypt/gnutls.c:386 | SONAME_LIBGNUTLS | cryptographic operations |
| crypt32.so | dlls/crypt32/unixlib.c:98 | SONAME_LIBGNUTLS | PFX import/export |
| secur32.so | dlls/secur32/schannel_gnutls.c:1479 | SONAME_LIBGNUTLS | Schannel TLS |

No Wine 10.0 Unix library directly dlopens libcrypto in this source tree.
This is a direct-call inventory, not a claim about transitive provider dependencies.
`winhttp` reaches Schannel through secur32; it has no direct GnuTLS dlopen site.

Scanned strings of every built `*.so` in
`harness/out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix`.
Exactly bcrypt.so, crypt32.so and secur32.so contain `libgnutls.so.30`;
none contain libcrypto. `ldd` on each shows libc and an unresolved ntdll.so,
with no GnuTLS/libcrypto DT_NEEDED entry: these providers are loaded at runtime.
The standalone ldd ntdll result reflects Wine's private library layout.
Raw source matches and strings/ldd output are saved under
`/workspace/x64-run/lanes/B44/o1/{source-inventory-full,source-inventory,binary-inventory}.txt`.

## Reproduction

Run `bash x64/uwp-host/harness/tls-evidence/repro.sh` from this worktree.
It uses the existing x64 `/workspace/xbox-readiness/bin/net_probe.exe`, the
project's `x64/pe/launcher/run_via_wine.c`, Wine's own bundled W^X loader,
and `libuwph_wine_shim.so`. It uses separate fixture prefixes and an external
Linux wineserver. This is THROUGH-uwph off-device harness evidence, not a
stock-Wine-only run, not build-only, and not proof of an in-process Xbox server.
No credentials are supplied. Certificate validation is left enabled.

`UWPH_DENY_DL=gnutls` is a Linux Wine harness-only opt-in: the dlopen wrapper
rejects basename `libgnutls.so` and versioned `libgnutls.so.*` before calling the
native loader. It obtains a native dlerror using a nonexistent test path.
Unset or other values preserve native dlopen behavior. No installed libraries
are renamed or removed; neither src/dl.c's static registry nor the Win32
backend is changed. The general shim's export surface remains unchanged.

| Run | HTTPS result | Failing call | Probe outcome |
| --- | --- | --- | --- |
| normal | HTTP 200, 993 bytes in final run | none; WebSocket upgrade also HTTP 101 | pass=7 fail=0 |
| deny | HTTP 0, 0 bytes; error 2148074245 (0x80090305) | WinHttpSendRequest → ensure_cred_handle → AcquireCredentialsHandleW | pass=5 fail=2 |

Deny debug output records three refused `libgnutls.so.30` loads and Wine's
bcrypt, crypt32 and secur32 provider-unavailable diagnostics. The later
WebSocket failure follows the absent directory response. `normal.log`,
`deny.log`, corresponding `*-debug.log`, `.exit`, `.exec` and `.counters.*`
record outputs, process exits, launcher/loader path and uwph routing counters.
The final rerun's response size may vary with directory content.
Artifact hashes are in `artifacts.sha256`. Earlier loopback-only logs were
superseded because that binary did not test HTTPS.

A free Xvfb display and ffmpeg record the paired runs to
`/workspace/x64-run/videos/B44-O1.mp4` (55 seconds). This video and all results
are Linux stand-ins; no Xbox console PASS is claimed.
