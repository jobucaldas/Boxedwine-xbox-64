# O2: owned content and QR authentication

The O1 `steamdl.exe` now also accepts:

```
steamdl.exe <fresh-output-directory> --token-file <private-staged-input-path>
steamdl.exe <fresh-output-directory> --qr
```

Both select app 3405340 (Megabonk), public Windows/English depots. The original
anonymous invocation still selects app 1007. Authentication uses Steam CM
WebSocket TLS, SteamClient-platform refresh login, and server-authorized depot
keys/manifest request codes. No Valve/game binary is executed or modified.
Every downloaded file is checked for manifest size and SHA-1 after reconstruction
and again from disk; chunks also retain their SHA-1 and compression CRC checks.
Outputs are under `out/megabonk-native/<depot>/`, with manifests and the expected
size/hash TSV in `out/megabonk-native`. DLC depots fail closed pending explicit
ownership selection. The existing per-file allocation bound remains 256 MiB; files are currently
assembled in memory, so this still needs a streaming port for console memory.

The plaintext token export option has been removed from
`x64/steam-min/seed-login.py`. Lane orders prohibit copying real refresh tokens;
mode 0600 and eventual deletion do not authorize a plaintext staging file.
`test-owned.sh` is disabled and exits 1 before builds, credential reads, copies,
or network activity. No owned-download acceptance proof is established.
The QR implementation is unchanged and was not exercised in this review fix.

QR requests explicitly set SteamClient (1) in both platform fields. The only
authentication data displayed is the `https://s.team/q/...` challenge URL (suitable
for rendering by the console UI). Request IDs, refresh/access tokens, account
names, and raw server errors are never displayed. Challenge rotation is handled;
polling respects the server interval and stops after three minutes without
confirmation. The user must approve the challenge in their own Steam mobile app.
No password flow is implemented.

`test-owned.sh` reports BLOCKED; do not use it as a runnable acceptance proof.
`test-qr.sh` is outside this credential-free review fix. Neither script establishes
Xbox execution. Desktop WinHTTP remains the transport; the documented WinRT
host transport discussed in DECISION.md is not implemented here.
No UWP networking/console PASS is established.

Protocol references checked 2026-10-25:

* [Steam authentication protobufs](https://github.com/SteamTracking/Protobufs/blob/master/steam/steammessages_auth.steamclient.proto)
* [SteamKit SteamUser login implementation](https://github.com/SteamRE/SteamKit/blob/master/SteamKit2/SteamKit2/Steam/Handlers/SteamUser/SteamUser.cs)
* The corresponding installed `steam-user/protobufs` declarations were used to
  check message IDs and field numbers. No private client DLL interfaces are used.
