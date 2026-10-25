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

## O2: provider options and decision

Need: Wine 10's bcrypt.so, crypt32.so and secur32.so look up 97 GnuTLS entry
points between them with dlsym (`tls/wine_gnutls_symbols.h`, checked against
the Wine source by `tls/check-symbols.sh`). That covers secur32's Schannel TLS
engine (about 40 symbols), bcrypt's asymmetric keys, sign/verify, DH and
ciphers (about 50), and crypt32's PFX import (about 12). Certificate
validation does not happen in GnuTLS. winhttp's `netconn_verify_cert`
calls Wine's crypt32 chain engine (`CertGetCertificateChain` +
`CertVerifyCertificateChainPolicy`), which verifies signatures through
bcrypt, so bcrypt.so needs the provider too. The tree is GPL-2
(`license.txt`).

| | (a) GnuTLS cross-built for x86_64-w64, linked into uwph's Win32 backend | (b) mbedTLS or BearSSL behind a gnutls-API shim | (c) Host WinRT StreamSocket TLS via a uwph unixlib |
| --- | --- | --- | --- |
| Licence vs GPL-2 tree | OK. GnuTLS and the bundled libtasn1 are LGPL-2.1+. nettle, GMP and the bundled libunistring are LGPL-3+ or GPL-2+, used under GPL-2+. The GPL-3 GnuTLS tools are not built. | mbedTLS: Apache-2.0 is NOT compatible with GPL-2-only code. Current releases also offer GPL-2.0-or-later, except for the Apache-only `3rdparty/everest` files and the 2.26 to 2.28.5 releases, so every file would need a licence audit. BearSSL: MIT, OK. | OK. These are platform APIs; nothing is linked in. |
| Cert validation | Unchanged Wine path: crypt32 chain engine, bcrypt signature checks through the provider, winhttp policy. Roots come from crypt32's store; on Xbox the documented crypt32 root store is the source (see follow-ups). | Same Wine path, but only if the shim's bcrypt sign/verify, key import and DH are reimplemented correctly. That means a security-sensitive rewrite of about 97 functions. | Host validation against Windows roots inside StreamSocket. It bypasses Wine's chain engine, so the result can't be reported per certificate through SSPI. |
| Coverage | All three call sites, unmodified Wine source. | All three only if the shim covers bcrypt and crypt32, not just TLS. BearSSL has no TLS 1.3, and its last release was 0.6 (2018). | secur32 only, and badly: SSPI is token-in/token-out over the caller's own socket, while StreamSocket owns the socket and never exposes TLS records. winhttp's transport would need replacing (a Wine fork). Games using Schannel on their own sockets, bcrypt asymmetric and crypt32 PFX stay unsolved. |
| Size | Measured: about 2.5 MB stripped PE (`provider_test.exe` 2,683,904 bytes vs uwph `smoke.exe` 169,472 bytes). The Linux self-contained .so is 2,960,784 bytes stripped. | Smaller library (mbedTLS about 0.5 to 1 MB, BearSSL about 0.3 MB) plus the shim. | Near zero. |
| Effort | Low: done in this spike. Pinned build script, a 30-line registry entry, no Wine changes. | High: reimplement about 97 GnuTLS APIs (privkey/pubkey/x509/pkcs12/DH/AEAD) and keep them in sync with Wine. | High, with partial coverage and async WinRT plumbing from a unixlib. |

**Decision: (a)**: GnuTLS 3.8.9 (with nettle 3.10.1, GMP 6.3.0 and the
bundled libtasn1), cross-built with x86_64-w64-mingw32, linked into the uwph
Win32 backend, and registered in uwph's static dl registry (`src/dl.c`) as
`libgnutls.so.30`. Wine's three Unix halves then find it through
`uwph_dlopen`/`uwph_dlsym` with no source change. Certificate validation
stays in Wine's crypt32/winhttp path and is never disabled. (b) is rejected
for its size of security-critical reimplementation (and mbedTLS for the
licence point above). (c) is rejected because it cannot back SSPI and
misses bcrypt and crypt32.

### Spike

Files: `x64/uwp-host/tls/`:

- `build-gnutls.sh`: fetches pinned releases and checks sha256. Upstream
  signatures were checked with gpg against `gnu-keyring.gpg` (Daiki Ueno,
  Niels Möller, Simon Josefsson); the result is in
  `lanes/B44/o2/sig-verify.txt`. It builds with no trust store, p11-kit,
  IDN, compression, TPM or tools.
- `gnutls_provider.c`: the registry entry.
- `provider_test.c`: the Win32 test.
- `cert_probe.c`: the bad-chain control.
- `Makefile`, `check-symbols.sh`.

Linux harness: `UWPH_TLS_PROVIDER=<absolute path>` (opt-in, test-only, like
`UWPH_DENY_DL`) makes the shim answer Wine's `libgnutls.so*` dlopen with the
uwph-built provider. It never searches or falls back to host GnuTLS. If
unset, behaviour is unchanged.

1. **Win32 backend (the Xbox path), off-device:** `make -C x64/uwp-host/tls test`
   cross-builds the provider and links it with `out/win/libuwph.a`. Running
   `provider_test.exe` under wine64 gives 9/9 PASS:
   - `uwph_dlopen("…/libgnutls.so.30")` comes from the static registry.
   - `uwph_dlsym` resolves 97/97 Wine symbols, and `gnutls_global_init`
     works through it.
   - An in-memory TLS 1.3 handshake (AES-256-GCM) runs against a generated
     ECDSA CA, with X.509 verification ON. Trusted chain: status 0. Untrusted
     issuer: 0x42 (SIGNER_NOT_FOUND). Wrong host: 0x4002 (UNEXPECTED_OWNER).

   This proves build, link and registry work. It does not run Wine's own
   secur32 code in the PE build, and it is not a console PASS.
2. **Linux stand-in, THROUGH uwph:** `x64/uwp-host/harness/tls-evidence/spike.sh`
   uses our `run_via_wine` launcher, Wine's own W^X loader and
   `libuwph_wine_shim.so`, with `UWPH_DENY_DL=gnutls` plus
   `UWPH_TLS_PROVIDER=tls/out/linux/lib/libgnutls.so.30`. That provider is
   built from the same pinned sources, as one object whose only dependency
   is libc.
   - net_probe: HTTPS GetCMList **HTTP 200**, WebSocket 101, pass=7 fail=0.
     net_probe sets no `SECURITY_FLAG_IGNORE_*`.
   - Bad-chain control: `badssl.com` returns 200. `self-signed.` and
     `untrusted-root.` are rejected with 12045, `expired.` with 12037, and
     `wrong.host.` with 12038 (all in the `netconn_verify_cert` trace).
     WinHTTP surfaces all of these as 12157, Wine 10's
     `ERROR_WINHTTP_SECURE_CHANNEL_ERROR` mapping in `dlls/winhttp/net.c`.
   - The strace openat log shows no host libgnutls, nettle, hogweed, gmp,
     tasn1 or p11-kit file opened. All 3 Wine dlopens per process go to the
     provider.
   - Control without the provider: HTTP 0, 0x80090305, as in O1.

   `spike.sh` enforces these results with `verify-spike.py`: unexpected
   exit codes, missing success/rejection evidence, or successful host TLS
   dependency opens make the spike target fail. Logs remain available on failure.

   Evidence: `/workspace/x64-run/lanes/B44/o2/`. Video:
   `/workspace/x64-run/videos/B44-O2.mp4` (55 s, free Xvfb display).

### Follow-ups (not done; Xbox remains SKIP)

- Import surface added by GnuTLS: the CryptoAPI (`CryptAcquireContextW`
  etc.), NCrypt, CertOpenStore/PFXImportCertStore, BCryptGenRandom, ws2_32,
  synch, TLS and file functions. With `WINAPI_FAMILY_APP` mingw-w64 headers,
  all of them are exposed except **`CreateFileA`**, which comes from gnulib's
  `stat` replacement (`libgnu_la-stat.o`). That object must be built for the
  APP family or replaced before packaging. mingw's partitioning is not
  authoritative; check this list against the official UWP Win32 API list.
- Root store on Xbox: Wine's crypt32.so reads Unix root files. Feed it from
  the documented host crypt32 ROOT store through uwph, or ship a CA bundle.
- Not yet built: Wine's own bcrypt/crypt32/secur32 Unix code compiled for
  the Win32 backend and run against this registry entry.
