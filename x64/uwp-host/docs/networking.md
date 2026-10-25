# Networking on Xbox

Console behavior is **unconfirmed until it runs on the target Xbox**. The
Windows smoke test runs the mingw-built uwph Windows code under stock Wine (its
ws2_32 hosts the Winsock calls). It is an off-device standin, not a console PASS.

## What uwph does on the Win32 (UWP) backend

| Socket kind | Transport | Code |
|---|---|---|
| `AF_UNIX` stream, `socketpair(AF_UNIX)`, pipes (wineserver transport) | in-process endpoints, no OS sockets. Unchanged by B42 | `src/socket.c` `uwph_socketpair` / `make_pair` |
| `AF_INET` / `AF_INET6` `socket()` and everything on those fds | Winsock (ws2_32), one-time `WSAStartup(2.2)` | `src/inet.c` `uwph_inet_socket`, `uwph_inet_call` |
| `socketpair(AF_INET, SOCK_STREAM)` | loopback listener fallback (below) | `src/inet.c` `uwph_inet_socketpair` |
| `getaddrinfo` / `freeaddrinfo` | Winsock `getaddrinfo`, copied into the glibc layout | `src/inet.c` `uwph_getaddrinfo` |

`socketpair(AF_INET6, …)` returns `EAFNOSUPPORT`. `socketpair(AF_INET, SOCK_DGRAM)`
returns `EPROTONOSUPPORT`. The Linux passthrough backend is not affected; it
still calls the host's `socketpair`.

### AF_INET socketpair: loopback listener

Winsock has no `socketpair`. `uwph_inet_socketpair` builds one inside our own
process:

1. Bind a listener to `127.0.0.1` on port 0, `getsockname`, `listen(1)`.
2. Bind a client socket to `127.0.0.1` on port 0, then `connect` to the listener.
3. `accept`, and keep the accepted socket only if its peer address equals the
   client's own address. Any other connection is closed (at most 4 tries).
4. Close the listener, set `TCP_NODELAY` on both ends, and wrap both as uwph
   INET fds (`SOCK_NONBLOCK`/`SOCK_CLOEXEC` honoured). `sv[0]` is the client
   and `sv[1]` the accepted end.

The pattern is adapted from BoxedWine-uwp (GPL-2), `Platform::nativeSocketPair`
([platform.cpp#L320-L370](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/platform/windows/platform.cpp#L320-L370)).
That is the code BoxedWine's Wine uses for its socketpairs on Xbox. Two
differences from it:
* There is no `SO_REUSEADDR`, because on Winsock it lets another socket take a bound port.
* Neither end keeps a port in 57344-65535. Port 0 can return any port in the
  Windows ephemeral range 49152-65535, so `loopback_bound` keeps a rejected
  socket open (at most 16) and binds again until it gets a port below 57344.
  It never binds into the documented drop range on purpose.

Smoke test: `inet_socketpair` in `tests/smoke.c` `test_inet`. It checks echo in
both directions, that both ports are below 57344, EOF after one end closes,
`SOCK_NONBLOCK` → `EAGAIN`, and the two rejected cases.

## Evidence (DOCS-ANSWERS rows 13 and 15)

**Row 13: loopback inside one app.** CONFIRMED-BY-CODE for intra-app use,
CONFIRMED-BLOCKED for app-to-app use.
* Code: BoxedWine-uwp `socketpair()` is a TCP listener on `INADDR_LOOPBACK`
  port 0 plus connect+accept
  ([platform.cpp#L320-L370](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/platform/windows/platform.cpp#L320-L370)).
* Doc: "Windows disallows establishing a socket connection … between two
  Windows apps running on the same machine; whether that's via the local
  loopback address (127.0.0.1)". The same page's sample runs client and server
  "in the same process" on localhost
  ([sockets](https://learn.microsoft.com/en-us/windows/uwp/networking/sockets)).
* Doc: "loopback connections for IPC are blocked by default for packaged
  applications", meaning between packages
  ([IPC](https://learn.microsoft.com/en-us/windows/uwp/communication/interprocess-communication)).
* Consequence: both ends of an AF_INET socketpair live in our one process, which
  is the allowed case. Nothing may expect a second app on loopback. The
  in-process wineserver uses AF_UNIX endpoints and needs no loopback at all.

**Row 15: Winsock and blocked ports.** CONFIRMED-OK with one documented restriction.
* Code: Xenia UWP uses `WSAStartup`/`getaddrinfo`
  ([socket_win.cc#L30-L51](https://github.com/SirMangler/xenia/blob/e59f1ad08d47cf227bcac80c88a0c527121ddcbb/src/xenia/base/socket_win.cc#L30-L51)).
  Most Xbox UWP manifests (Unleashed, BoxedWine, PPSSPP, Dolphin, RetroArch)
  declare `internetClientServer` + `privateNetworkClientServer`.
* Doc: the UWP Win32 API list includes the ws2_32 basics (`socket`, `bind`,
  `connect`, `getaddrinfo`, `WSAStartup`, …)
  ([win32-apis](https://learn.microsoft.com/en-us/uwp/win32-and-com/win32-apis)).
* Doc (Xbox known issues): apps are "restricted from binding to ports in the
  range [57344, 65535] … traffic can be silently dropped … bind to port 0
  wherever possible … must be in the range [1025, 49151]"
  ([known-issues](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/known-issues)).
* Doc: `internetClient` "Cannot act as a server"
  ([capabilities](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations)).
  A local listener (including this socketpair fallback) therefore needs
  `internetClientServer` in the manifest (B45).

How uwph applies row 15:
* An explicit `bind` to a port ≥ 57344 fails with `EACCES` and logs
  `bind refused: Xbox known issue: …` (`src/inet.c`, `uwph_inet_call` `INET_BIND`).
* The socketpair fallback keeps both of its ports below 57344 (above).
* Open gap: implicit binds (port 0 `bind`, or `connect` without `bind`) by
  other callers still take whatever ephemeral port Windows assigns, which may
  be ≥ 57344. Not observed on a console yet.

Out of scope here: TLS (B44). The UWP Win32 list has no winhttp/schannel
entries (DOCS-ANSWERS row 16).
