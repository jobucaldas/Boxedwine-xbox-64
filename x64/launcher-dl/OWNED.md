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

Only `x64/steam-min/seed-login.py` opens the original token JSON. Its added
`--launcher-token-out` mode writes a private, exclusive-create, mode-600 staging
file under its gitignored `out/`. The test moves that file under launcher `out/`
for the sandbox, passes only its path to the PE, and removes it on exit/signals.
The PE reads the staged platform/account/SteamID/refresh fields from that path;
no token is passed on a command line or printed. The reader is the B50 O2 script,
carried into this lane with its original login-cache behavior preserved. Do not
run its cache-seeding mode for this test. The QR path holds its result only in
memory and proceeds directly to the same owned login/download code. It does not
persist a new login, so a future launch without staged input requires another QR.

QR requests explicitly set SteamClient (1) in both platform fields. The only
authentication data displayed is the `https://s.team/q/...` challenge URL (suitable
for rendering by the console UI). Request IDs, refresh/access tokens, account
names, and raw server errors are never displayed. Challenge rotation is handled;
polling respects the server interval and stops after three minutes without
confirmation. The user must approve the challenge in their own Steam mobile app.
No password flow is implemented.

Run `bash x64/launcher-dl/test-owned.sh` for the exact owned-download proof.
It builds, prepares a stock Wine prefix at packaging time, then runs THROUGH
`uwp_run.sh(nofork,wx,noproc)` -> our launcher -> Wine's own W^X PE loader ->
uwph with Wine's server hosted in process. It requires a fresh output directory;
archive an earlier output before rerunning. `test-qr.sh` exercises QR session
creation without opening the token file; it intentionally times out without
mobile confirmation. Neither script claims Xbox execution. Desktop WinHTTP is
still the transport; the documented WinRT host transport discussed in DECISION.md
is not implemented here. No UWP networking/console PASS is established.

Protocol references checked 2026-10-25:

* [Steam authentication protobufs](https://github.com/SteamTracking/Protobufs/blob/master/steam/steammessages_auth.steamclient.proto)
* [SteamKit SteamUser login implementation](https://github.com/SteamRE/SteamKit/blob/master/SteamKit2/SteamKit2/Steam/Handlers/SteamUser/SteamUser.cs)
* The corresponding installed `steam-user/protobufs` declarations were used to
  check message IDs and field numbers. No private client DLL interfaces are used.
