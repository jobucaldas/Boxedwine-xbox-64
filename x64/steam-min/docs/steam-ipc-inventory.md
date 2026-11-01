# Steam engine process and IPC inventory (B51 O1)

Status: **incomplete / blocked**. B50's seeded stock-Wine and THROUGH-uwph
attempts both failed the login gate after 120 seconds. Neither ran the probe
or game. There is no successful SteamAPI_Init baseline from which to measure
child-denial dependencies or identify game-to-engine IPC. The required section
test checks document structure; passing it does not establish a complete inventory.

Evidence: `/workspace/x64-run/lanes/B50/o2/` (stock), `B50/o3/`
(THROUGH uwph), and `/workspace/x64-run/lanes/B51/o1/{stock,uwph}/`.
B51 uses fresh, separate prefixes with **no credentials**, no token access and
no login seeding, as required by the task context. Only Valve's manifest-verified
packages are installed; CEF is absent. Reproduce after fetching the same minimal
package set into `x64/steam-min/out/client` and building the three harness targets
used by B50: `bash x64/steam-min/inventory-startup.sh`.

Each B51 startup is bounded at 45 seconds and uses
`WINEDEBUG=+process,+loaddll,+sync,+winsock`, plus `strace -ff` process,
bind/listen/getsockname/connect records. Prefix setup is stock Wine, outside the
proof. The second startup uses this worktree's `run-wine.sh --guest` ->
`uwp_run.sh wx,noproc` -> our launcher -> Wine's own private W^X loader -> shim
-> uwph, with **nofork OFF** and an external Wine server. This is Linux harness
startup evidence, not console proof or successful game execution.

## Processes

The table includes successful and attempted children; CreateProcess requests
alone are not proof of a running child. Parent assignments for Valve helpers
follow the caller and command line in the process trace; Wine infrastructure
is listed separately. Raw per-route process records are in `processes.txt`.

| Executable | Parent / role | Evidence | SteamAPI_Init if denied by --nofork |
|---|---|---|---|
| steam.exe | launcher/Wine; stock trace also records self launch | B50 stock and routed startup; B51 | Unknown; engine/game baseline absent |
| steamsysinfo.exe | steam.exe diagnostic worker | B50 both routes; B51 stock | Unknown |
| steamerrorreporter64.exe | steam.exe (`-pid` identifies engine) | B50 stock; B51 stock | Unknown |
| gldriverquery.exe | steam.exe graphics query | B50 stock; B51 stock | Unknown |
| gldriverquery64.exe | steam.exe graphics query | B50 stock; B51 stock | Unknown |
| vulkandriverquery.exe | steam.exe graphics query | B50 stock; B51 stock | Unknown |
| vulkandriverquery64.exe | steam.exe graphics query | B50 stock; B51 stock | Unknown |
| steamwebhelper.exe | steam.exe; **attempt only**, binary absent | B50 stock; B51 stock | Unknown; absence plus failed login does not establish causality |
| wineboot.exe | Wine startup, launched by initial guest loader | B51 startup | Unknown; infrastructure, not Valve child |
| winemenubuilder.exe | wineboot.exe | B50/B51 startup | Unknown |
| services.exe | wineboot.exe | B50/B51 startup | Unknown |
| winedevice.exe (multiple) | services.exe | B50/B51 startup | Unknown |
| plugplay.exe | services.exe | B50/B51 startup | Unknown |
| svchost.exe | services.exe; LocalServiceNetworkRestricted | B50/B51 startup | Unknown |
| rpcss.exe | Wine service startup (winedevice caller in B51 stock) | B50 stock; B51 stock | Unknown |
| conhost.exe | steamsysinfo / driver query console children; other Wine console startup | B50/B51; per-caller records retained | Unknown |
| winedbg.exe | Wine crash/debug path; target PID in command line, precise crashing parent not resolved in B50 summary | B50 routed | Unknown |
| wineserver | external Linux Wine infrastructure | B51 syscall trace / harness setup | Unknown; not a Windows Steam child |

No per-child denial experiment is claimed. `--nofork` globally denies process
creation, rather than selectively disabling one named child. Without a successful
unrestricted baseline, such failures cannot establish which helper is optional.
O2's in-process-server/no-fork experiment is outside this order and was not run.

## IPC objects

**Game <-> engine:** no observed objects, because the game/probe never ran in
B50 and B51 has no credentials. Named shared memory/sections, events, mutexes,
semaphores, pipes and socket endpoints for that relationship remain **unknown**.
Do not interpret this as an empty required IPC set or infer names from binary
strings. The requested debug channels also do not exhaustively report every
NT section/event/mutant creation; additional server/object tracing and endpoint
correlation would be needed after a valid baseline is available.

**Startup infrastructure:** B51 stock observes named pipes `\\.\pipe\svcctl`,
`\\.\pipe\net\NtControlPipe0` through `NtControlPipe4`,
`\\.\pipe\wine_plugplay`, `\\.\pipe\epmapper`,
`\\.\pipe\lrpc\epmapper`, `\\.\pipe\lrpc\irot`,
`\\.\pipe\lrpc\irpcss`, and a per-process OLE LRPC pipe. These are Wine
service/RPC plumbing, not proven Steam game IPC. The exact names and callers
for both routes are retained in `named-pipes.txt`. Unnamed sync handles are
present, but handles alone do not identify a shared named object or its peers.

The syscall trace also observes Wine's AF_UNIX `socket` server endpoint and
AF_NETLINK interface notifications. Neither has a TCP/UDP port. Socket endpoints
below are engine startup observations; no game peer is established.

## Ports

All explicit successful Windows TCP/UDP binds in the bounded captures are
listed below and backed by `ports.txt`; port 0 requests are paired with the
actual assigned port from Wine's successful-bind line. Ephemeral allocations
can change on subsequent runs. Outbound implicit binds and later logged-on
activity are not an exhaustive part of this startup capture.

| Route | Transport | Local address | Requested port | Assigned port | In 57344–65535? |
|---|---|---|---|---|---|
| Stock | TCP | 127.0.0.1 | 0 | 39685 | No |
| Stock | TCP | 127.0.0.1 | 0 | 38999 | No |
| Stock | TCP | 0.0.0.0 | 0 | 39721 | No |
| Stock | TCP/IPv6 | :: | 0 | 35669 | No |

The two loopback TCP sockets call listen successfully. Their purpose is not
established as game IPC. A UDP socket creation is visible, but is not itself a
UDP bind. See the routed results appended below for its observed bind set.
B50's connection log contains remote HTTP port 80 connectivity attempts;
these are remote destinations, not local binds. The 57343/57344 ports mentioned
in STEAM-MINIMAL.md belong to the separate GameNative Android design and are
not observations of Valve's Windows client here. No Xbox port behavior is tested.

### THROUGH-uwph capture result

The routed capture reports `uwpjail: seccomp wx=1 nofork=0 (23 insns)`
and the launcher handing Steam to this worktree's private Wine loader.
It hits `start_rpcss Failed to start RpcSs service`, invalid heap handles,
two winedbg launches and service command timeouts before any successful
Windows socket bind is logged. Its bind set is **none observed in this
bounded startup**, not a claim that the engine needs no ports. The timeout
ends with exit 137 after the kill grace period (stock ends with exit 124).
No uwph counter report was captured (the original report destination was outside
the guest writable tree, and termination was forced); route evidence is
the jail/launcher log, not completed backend counters. No successful engine
login, game load, probe call or SteamAPI_Init is claimed for this rerun.

The stock startup binds above are confirmed by matching AF_INET/AF_INET6
bind/getsockname records in the external Wine server's syscall trace. The
requested bind port is 0 for all four. No explicit UDP bind or assigned
port in 57344–65535 is observed in either bounded run; later activity and
implicit outbound source ports remain unmeasured. AF_UNIX/AF_NETLINK
records are excluded from the TCP/UDP table.

### Remaining evidence needed

A legitimate successful login and unmodified-game SteamAPI_Init baseline
is needed before an exhaustive game-to-engine object/endpoint inventory and
child-denial dependency matrix can be produced. This order does not authorize
using credentials against the user's explicit “No real credentials” constraint.
No alternate credential path, private interface, binary modification, CEF
installation or verifier change was attempted. The document therefore preserves
the blocker rather than inventing successful child-denial results.

Video: `/workspace/x64-run/videos/B51-O1.mp4`, 8 seconds on the free Xvfb
display recorded in `route.txt`; it displays the blocked-inventory status,
not game progress. Helper/harness compilation is build-only; the two startup
routes are classified separately above. No console PASS is asserted.
