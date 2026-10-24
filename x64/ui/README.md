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
timeout retains child status 124 and returns adapter error/exit 1, never the
known blocker. The Linux worker starts a new session; on timeout the adapter
sends TERM to its process group, allows two seconds for harness cleanup, then
sends KILL to remaining group members and reaps the worker while retaining
merged output. This leaves the upstream verifier and acceptance gate unchanged.

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

O2 unit tests include injected process results, asynchronous future submission,
and a real subprocess timeout/descendant cleanup regression with retained logs
and status. These are HOST-ONLY behavior tests, not live routing evidence. The CLI launch is the live THROUGH-layer test with
stock controls. Visible shell and Xvfb video are O3/O4 deliverables; O2 provides
no visible UI to record. No real authentication, depot download, account,
credential, or network behavior is implemented.

O3 supplies the working standard-library curses shell and `MainPage.xaml` design
reference. Run `python3 x64/ui/app.py` in a terminal (or xterm). Approve the
explicit offline login placeholder with Enter or A. Left/right moves between
three mock tiles; Down moves to Install, left/right switches Install/Launch,
and Up returns to the selected tile. Escape or B returns to login, then exits.
Focus is marked `>`; the details, busy/result status and latest log lines stay
visible. These are keyboard/controller-style bindings, not physical gamepad
support. Resize to a larger terminal for more logs; complete launch output is
retained by O2 in `out/launch-*/child.log`. Exiting waits for a pending bounded
adapter worker to finish; navigation stays responsive during work.

Both visible buttons and headless scenarios call `Shell.dispatch`, which
submits Install and Launch to a worker. Launch uses O2 `launch_async`, with no
alternate direct-Wine path or simulated success. Only hello-demo is supported;
fictional tiles pass their own IDs to the adapter and return unavailable.
Install prepares our fixture and downloads nothing. Launch is the existing
live multi-PE harness batch, not a single installed game. A blocked result is
shown as BLOCKED with the adapter's raw evidence and exit code preserved.

```sh
python3 -m unittest discover -s x64/ui/tests -p 'test_ui_flow.py' -v
python3 x64/ui/app.py --headless --scenario navigation
python3 x64/ui/app.py --headless --scenario launch-hello
make -C x64/uwp-host all
git diff --check
```

Navigation and injected UI unit tests are HOST-ONLY. The live launch scenario
uses THROUGH-layer runs (our run_via_wine -> Wine loader -> shim -> selected
uwph Linux operations), plus separately identified stock Wine controls and an
unrouted stock wineserver. Current expected exit is 2 / LAUNCH BLOCKED for the
unchanged wineboot-attempt gate, even with positive selected counters. No Xbox
execution, UWP package, real Steam login/download or physical gamepad is proved.

`MainPage.xaml` is an uncompiled WinUI 3 desktop design reference: its buttons
and status text are illustrative, with no event bindings or build claim. A UWP
port needs a separate UWP project, compatible XAML controls/properties and
spacing replacements where required, explicit focus/controller wiring,
UI-thread dispatch of asynchronous results, platform credential-store access
and validation of documented capabilities. Linux Python subprocess management
must be replaced with the intended in-process Wine loader integration; this
reference does not establish that integration or device/API authorization.

O3 validation: 3 UI tests passed; navigation exited 0; live launch exited 2
with the unchanged wineboot-attempt blocker; build and whitespace checks exited
0. Linux capture: `/workspace/x64-run/videos/R5-O3.mp4` (H.264, 35 seconds),
reviewed final frame `/workspace/x64-run/v2/R5/O3-final.png`. Display capture is
HOST-ONLY; its live launch evidence is `out/launch-kmh5tjaa/`, status blocked.

O4 records the visible, live demo on an exclusively owned Linux Xvfb display:

```sh
bash -n x64/ui/record.sh
x64/ui/record.sh O4
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -show_entries format=duration -of default=noprint_wrappers=1 /workspace/x64-run/videos/R5-O4.mp4
make -C x64/uwp-host all
git diff --check
```

The recorder refuses an occupied/reserved :99, starts Xvfb at 1280x720, runs
curses inside xterm and captures with ffmpeg x11grab/H.264. `--demo-evidence`
drives the actual `Shell.dispatch` with timed login approval, all three tiles,
Install and Launch. Screens/actions remain visible for two or more seconds;
the final blocked result holds for five seconds. No saved routing evidence is
used as UI input. Demo adapter children have a 20-second timeout plus cleanup;
the xterm has a 54-second timeout and recording a 58-second ceiling. Cleanup
signals only owned processes and finalizes capture on failed runs too.

Video: `/workspace/x64-run/videos/R5-O4.mp4`. Evidence lives under
`/workspace/x64-run/v2/R5/O4/`: raw launch/install and UI logs, separate
`launch.exit`, `ui.exit`, `xterm.exit`, `capture.exit`, scenario actions/result,
ffprobe metadata and `final.png`. `scenario.json` links the unique live O2
child log; that log references the unchanged harness's raw reports/traces.
Capture succeeds only for a completed expected blocked scenario, successful
UI/capture statuses and a nonempty H.264 video no longer than 60 seconds.
Unexpected launch failures retain their logs/status and fail capture acceptance.
The final screenshot must also be reviewed for readable UI and LAUNCH BLOCKED.

The footage is labeled “Linux prototype — Xbox validation pending”. Capture,
encoding and focus/navigation are HOST-ONLY. The live Launch button goes
THROUGH O2 -> existing harness -> run_via_wine -> real Wine loader/ntdll -> shim
-> selected uwph Linux backend operations. The same batch separately includes
launcher-only stock Wine controls and an unrouted stock Linux wineserver.
`launch.exit=2` is the unchanged wineboot-attempt gate, not runtime acceptance.
Capture success proves neither physical Xbox execution nor UWP compatibility.
