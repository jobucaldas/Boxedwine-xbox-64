# Offline Steam UI scaffold

O1 implements only standard-library Python contracts in `model.py` and three
fixed mock library entries. IDs are local identifiers, not Steam app IDs or
proof of ownership. No UI, QR code, runtime adapter, install or launch is
implemented in this order.

`OfflineSteamBackend.begin_login()` enters `MOCK_PENDING`;
`approve_mock_login()` explicitly approves the simulated session. Cancel clears
the session, and a new backend starts signed out. Only a mock signed-in session
can list the fixture library. Other login flow names (including `qr` and
`refresh_token`) return `UNAVAILABLE`; `download()` always returns
`UNAVAILABLE`. Install states distinguish not installed, future mock fixture
readiness, and unavailable operations. `SteamBackend` defines the future
connector boundary without supplying a live implementation.

No passwords, token fields, credential objects, network calls or state writes
exist. Mock approval establishes no device/API authorization. A future real
adapter would use QR approval and refresh-token authentication with João's own
account, retrieve the owned library and check entitlements before depot
downloads. Refresh tokens must be held by a platform credential store and
redacted from UI, logs and errors; this scaffold implements no secret storage.

The planned shell uses a controller-friendly grid, focus navigation and explicit
Install/Launch actions. WinUI 3 XAML targets desktop Windows. UWP needs a distinct
compatible XAML/project/API adaptation and documented capability validation;
there is no drop-in port or Xbox build/device claim here. The future launch
adapter must route owned hello.exe through the existing `run_via_wine` launcher
and `x64/uwp-host` harness, preserving its acceptance checks and blockers.
Linux subprocess orchestration cannot establish an in-process UWP launch model.

Validation from the repository root:

```sh
python3 -m unittest discover -s x64/ui/tests -p 'test_model.py' -v
make -C x64/uwp-host all
git diff --check
```

Proof classification: model tests and documentation are HOST-ONLY. The parent
command builds/checks uwp-host artifacts; it does not execute the uwp-host
runtime, our launcher or stock Wine. O1 supplies no launch routing proof or
video; visible UI/capture belongs to later orders. R4H's wineboot-attempt
acceptance blocker remains unresolved.

O2 adds `runtime.py`, a Linux-only process adapter. Mock install builds our own
hello fixture with the unchanged `x64/pe/tests/build.sh` in an isolated directory,
records its absolute path, readiness and SHA-256 under ignored `x64/ui/out/`,
and downloads no Steam content. Tracked fixture binaries are never overwritten.
Only `hello-demo` is supported; other IDs return unavailable.

```sh
python3 -m unittest discover -s x64/ui/tests -p 'test_runtime.py' -v
python3 x64/ui/runtime.py install --game hello-demo
python3 x64/ui/runtime.py launch --game hello-demo
make -C x64/uwp-host all
git diff --check
```

Launch checks installed integrity and the existing harness fixture hashes, then
runs exactly `make -C x64/uwp-host/harness wine-test` in a worker. UI callers can
use `Runtime.launch_async(game, executor)` and observe the returned future.
This is honestly a batch of hello.exe and hello_k32.exe routed and control runs,
using the harness fixtures, not execution of a selected downloaded game. The
installed copy demonstrates fixture preparation; the unchanged harness retains
its own fixture isolation and nonblocking lock. Concurrent launches can therefore
return a distinct harness error. Each completed batch retains raw merged child
output, child exit status and classified result in a unique `out/launch-*`
directory; its output links the harness's full raw traces and reports. A worker
timeout is an error (124), never the known blocker.

Live routed runs go THROUGH our compiled run_via_wine -> real Wine PE
loader/ntdll -> shim -> public uwph Linux backend for open/read/close/mmap/munmap.
Existing PID/module/counter verification is unchanged. Launcher-only stock Wine
controls produce no uwph reports; the separate Linux wineserver is also stock
and unrouted. The unchanged verifier rejects wineboot attempts even when all
four PEs exit zero and selected counters are positive. This exact rejection
returns `LAUNCH BLOCKED` / exit 2. Other failures return `LAUNCH ERROR` / exit 1,
with the original child status retained separately. Only upstream verifier PASS
and child exit zero yield `LAUNCH VERIFIED: selected Linux surface only`.
Python subprocesses establish no UWP process model or Xbox execution proof.

O2 unit tests inject process results: they are HOST-ONLY behavior tests and are
not live routing evidence. The CLI launch is the live THROUGH-layer test with
stock controls. Visible shell and Xvfb video are O3/O4 deliverables; O2 provides
no visible UI to record. No real authentication, depot download, account,
credential, or network behavior is implemented.
