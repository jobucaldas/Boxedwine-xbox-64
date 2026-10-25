# Boxedwine Xbox Developer Mode test kit (x64)

Status: no console proof exists yet. The package now starts `rungs.exe`, a native
CoreApplication/IFrameworkView implementation for O5. Its activation, SDK schema
validation and real Xbox API behaviour remain unverified. `smoke.exe` remains a
separate build diagnostic. Tomorrow's checklist:
`/workspace/x64-run/ON-DEVICE-TOMORROW.md`.

## Build the artifacts (Linux)

    make -C x64/on-device package        # loose payload + test cert
    make -C x64/on-device kit            # package + hosted ladder -> out/kit.zip
    make -C x64/on-device offline-smoke  # package, offline ladder, kit, Wine smoke
    bash x64/on-device/check-doc.sh      # this README vs the built kit

- Loose payload: `x64/on-device/out/payload/` (`AppxManifest.xml`, `rungs.exe`, `smoke.exe`, optional `probe_native.exe`,
  `Assets/*.png`, `PACKAGING-NOTES.txt`, and the hosted Linux `combo/` assets
  described under "B21 combo rung")
- Test certificate: `x64/on-device/out/test-cert.cer` (CN=BoxedwineDev,
  matches manifest Publisher)
- Private test key: `x64/on-device/out/test-cert.pfx` (empty password; this is
  a disposable dev identity. Never put it in the payload or share it.)
- Kit: `x64/on-device/out/kit.zip`, extracted as `x64/on-device/out/kit/`.
  Verify it with `cd x64/on-device/out && sha256sum -c SHA256SUMS`; the
  sidecar `out/SHA256SUMS` covers `kit.zip` and every extracted file. Top level:
  - `README.md` (this file), `KIT-NOTES.txt` (build-only notes),
    `kit-manifest.txt` (every file and the rung it serves) and `SHA256SUMS`
    (in-kit checksums, excluding itself).
  - `ladder`: the hosted Python ladder (see "Hosted ladder").
  - `payload/`: the loose payload above. Its `combo/` symlinks to Debian
    Wine files are stored as regular files in the zip.
  - `probes/`: optional diagnostic PEs copied from other worktrees' build
    outputs (`probes/<worktree>/graphics|native-gfx/`). They aren't wired into
    the console app.
  - `test-cert.cer` and `test-cert.pfx` (private; never upload the `.pfx`).

`check-doc.sh` checks that every rung, file, flag, make target and step in
this README and `/workspace/x64-run/ON-DEVICE-TOMORROW.md` exists in the built
kit, and that every kit entry and rung is documented here. It's build-only.

## Pack and sign (Windows PC with the Windows SDK)

Device Portal's Add flow takes a signed package. Copy `out/payload/`,
`out/test-cert.pfx` and `out/test-cert.cer` to the PC, then:

    MakeAppx pack /d payload /p BoxedwineDev.appx
    SignTool sign /fd SHA256 /f test-cert.pfx BoxedwineDev.appx

If MakeAppx rejects the manifest, record its exact error. Don't loosen the
capabilities to get past it. We haven't run either tool here.

## Install through Xbox Device Portal (one page)

1. Console is in Developer Mode. Open **Dev Home**, then under **Remote Access
   Settings** turn on **Xbox Device Portal** and set a Device Portal user name
   and password. These are local to the console, not your Microsoft account.
   Don't commit them anywhere.
2. Note the console IP shown in Dev Home. From a PC on the same LAN, browse to
   `https://<console-ip>:11443`. Accept the console's self-signed HTTPS
   warning, which is expected only for your own console on your LAN, and sign in.
3. **Home > My games & apps > Add**. Choose `BoxedwineDev.appx` and continue.
   There are no dependency packages. If it asks for a certificate, give it
   `test-cert.cer` (never the `.pfx`). Start the install and wait for it to
   finish.
   - Alternative: if your Device Portal offers loose-folder registration from a
     network share, register a share holding `out/payload/`. Only use your
     own LAN share.
4. Dev Home -> highlight app -> View button -> View details -> App type: Game.
   Device Portal: default new apps to Game. Run the ladder once as App and once
   as Game and keep both logs. Record the measured memory limit for each run.
5. Launch it from Dev Home. Record whether it activates, crashes, or exits. Also
   record any error code and the Device Portal crash/ETW output.
6. To remove it: **My games & apps**, then pick the app and **Remove**.

Out of scope: Store submission, real Microsoft signing, and any security,
hypervisor or code-signing bypass. Use documented capabilities only.

## Manifest and capabilities (O1)

`AppxManifest.xml` targets Windows.Xbox/x64 and declares `codeGeneration`
(base foundation capability), `internetClient`, `privateNetworkClientServer`
and restricted `expandedResources` next to `codeGeneration`. `expandedResources`
is deprecated and Store-rejected; this manifest is for dev sideload only.
Declaring a capability doesn't prove it's available on a given console OS.
The XML is well-formed but hasn't been validated against the SDK schema.
`make -C x64/on-device smoke-exe` builds `out/smoke.exe` with
`x86_64-w64-mingw32-gcc`.

## Offline smoke (O4)

`make -C x64/on-device offline-smoke` runs `pack.sh`, then
`out/ladder --offline`. After that it runs the packaged `out/payload/smoke.exe`
through our `run_via_wine` launcher (`x64/pe/launcher/run_via_wine.c`), which
hands it to Wine's own loader (`/usr/lib/wine/wine64`). That run uses a
disposable `/tmp` prefix, which is deleted afterwards. It shows that the
packaged PE runs under desktop Wine 10.0 on Linux. It doesn't show uwph
routing of smoke.exe, UWP activation, or Xbox behaviour.

## Hosted ladder (O2)

`make -C x64/on-device ladder && ./x64/on-device/out/ladder --offline` prints
26 STAGE lines (a–h, combo, p1–p6, n0–n8, n10, n11) and overwrites `LocalState/ladder.log`, flushing each
line. Use `--local-state PATH` to pick another LocalState directory. Adapter stages
time out after 20 seconds by default (`--timeout 1..60`); the offline combo
has a 60-second bound with 20 seconds per guest. A stage FAIL or SKIP
doesn't stop the run, and the exit code stays 0. An unwritable log is an error.
Offline mode runs stage a (public uwph Linux backend) and the combo rung.
Stages b–h and n0–n8 stay SKIP. n10 and n11 are always SKIP in the hosted
ladder; when the payload and desktop Wine are present it also copies their
`standin` sub-lines (see "Platform rungs n10 and n11" below). This is a hosted
Python tool, so it doesn't run on the console.

LocalState layout (Git-ignored):

- `ladder.log`: status lines and fixed reasons only.
- `adapters/a` through `adapters/h`: optional executable integration adapters.
- `private/steam.token`: an optional token you supply yourself. The ladder
  never ships, opens, prints or passes it on argv. If it's missing, stage g is
  SKIP.
- `payload/Megabonk.exe`: an optional payload you own. If it's missing, stage h
  is SKIP.

Without `--offline`, an installed adapter runs with LocalState as its working
directory, with no shell and with stdout/stderr discarded. Exit code 0 means
verified success, 77 means a prerequisite is unavailable, and anything else
is FAIL. A missing adapter is SKIP, never PASS. Required routing:

- a and d: public uwph (RW, then RX, then execute; never RWX).
- b, c, f, g and h: our launcher plus Wine's own loader.
- e: a real in-process server. The separate Linux wineserver harness can't
  satisfy this.
- f: the dzn adapter plus a real D3D12 device.
- g: the real `SteamAPI_Init` result, with no emulation and no DRM bypass.

Adapter exit codes are trusted inputs, not independent proof of routing.
Nothing is downloaded, and there is no stock-Wine fallback.

## Packaging (O3)

`make -C x64/on-device package` runs `pack.sh`. It generates the OpenSSL test
certificate on first use and checks the CER/PFX pair, the publisher and the
x64 identity. It also creates the PNG assets the manifest references and
stages `out/payload/`. LocalState, tokens and private keys are never staged.
No signature or AppxBlockMap is produced here, so pack and sign on Windows as
shown above.

## Native ladder (O5)

`make -C x64/on-device ladder package` builds `out/rungs.exe` and the hosted
`out/ladder`, then rebuilds the loose payload. On console, launch the packaged
app from Dev Home: the **Boxedwine Dev Mode** tile (EntryPoint `Boxedwine.App`,
executable `rungs.exe`). It activates a real CoreWindow and writes
`ApplicationData.Current.LocalFolder/ladder-native.log`, flushing each stage.
Save this file separately as `ladder-App.log` and `ladder-Game.log` before
relaunching; the native log is overwritten each run. Keep hosted a–h evidence
separately; these native probes do not prove the Wine compatibility runtime.

- n1: D3D11 hardware HRESULT, feature level, adapter and WARP fallback result.
- n2: D3D12 creation at FL 11_0, reported feature level and shader model.
- n3: real CoreWindow, BGRA8 FLIP_DISCARD, two buffers, #0078D7 clear,
  120 vsynced Presents with average/max frame milliseconds.
- n4: xaudio2_9.dll XAudio2Create and a one-second 440 Hz sine, consumed sample
  count; WASAPI ActivateAudioInterfaceAsync export availability. Listen on device;
  API success does not establish audibility or WASAPI endpoint activation.
- n5: MemoryManager.AppMemoryUsageLimit via RoGetActivationFactory (hand-declared
  ABI), then VirtualAllocFromApp reserve/commit in 256 MiB chunks up to 4608 MiB,
  freeing every allocation. Only the first page is touched: this measures commit
  acceptance, not physical residency. Runs last because an OS memory kill may
  interrupt it; a starting note is flushed before allocation.
- n6: GetGamingDeviceModelInformation raw IDs and Series S/X model.
- n7: SKIP unless `--big-file` is passed to the native executable (or to a hosted
  n7 adapter). Writes and reads a 2304 MiB file in verified LocalState, checks
  per-block tags and endpoint bytes, logs bytes/errors, deletes on close.
  This is an opt-in size/I/O probe, not a full-byte integrity test. Normal Dev
  Home activation of the normal tile skips n7. For console opt-in, launch the
  separately named **Boxedwine Big file opt-in** entry from Dev Home. Its manifest
  EntryPoint `Boxedwine.BigFile` reaches `IFrameworkView.Load`, which enables
  `--big-file` for this run only. No desktop argv, child process, or hosted adapter
  is needed. Select App/Game on this entry too, and keep both logs. The log records
  the received EntryPoint and opt-in before probes. This activation route is
  implemented but still requires console validation.

Packaged activation fails before any probe if ApplicationData LocalState lookup or
`ladder-native.log` creation fails; there is no TEMP fallback. The original HRESULT
and CRT errno are emitted to debugger diagnostics/stderr and the HRESULT is returned
as the process exit code (inspect Device Portal diagnostics if no log exists).

If `/workspace/wt/R4N/x64/native-gfx/out/probe_native.exe` exists, packaging copies
it unchanged next to `rungs.exe`. n0 can run that copy in the desktop diagnostic;
on UWP it reports SKIP because launching child executables with CreateProcess is
unsupported. **O5 INCOMPLETE for coordinator:** an in-process R4N adapter is still needed for console n0 execution.
No child-process capability or bypass is requested.

Off-device `out/ladder --offline` keeps all n0–n7 SKIP. `./x64/on-device/native-smoke.sh` runs the native executable through our `out/run_via_wine` launcher into Wine's own loader with
`out/payload/rungs.exe --console`; successful stand-in calls are labelled SKIP,
never console PASS. Large commit and file tests require verified Xbox activation.
Hosted `adapters/n0`–`adapters/n7` follow the existing exit-code contract; only n7
receives `--big-file`, and offline mode never executes these adapters.

ABI references: [Microsoft SDK gaming IDs](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/gamingdeviceinformation.h)
and [MemoryManager ABI](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.system.h).

Activation API reference: [IFrameworkView.Load](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.core.iframeworkview.load).

## Troubleshooting

1. Before retrying, copy the complete error text and numeric code (HRESULT or
   Win32 error), including all detail lines. Record the run time, console OS
   version, selected tile, and App/Game setting. Keep partial logs before the
   next launch overwrites them. These are likely causes, not console diagnoses.
2. In Device Portal, open **File Explorer**, select the installed Boxedwine app,
   then open **LocalState**. Paths below are relative to that app's data folder;
   `LocalState/ladder-native.log` is the native console log.
   `LocalState/ladder.log` belongs to the hosted ladder and is present only if
   that evidence was explicitly placed there; the console app does not create it.
3. Use the matching row below. If a named diagnostic page or download is absent
   on your Device Portal version, record that fact and the page title you see.
   Do not change capabilities or bypass installation checks to clear an error.

| Symptom | What to copy back (exact file or Device Portal page) | Likely cause |
| --- | --- | --- |
| 1. Install says certificate is not trusted | **Home > My games & apps > Add**: full failed-install details and error code; record which public `.cer` was selected. | Test certificate is not trusted by the console, or signing certificate and manifest Publisher do not match. The portal HTTPS certificate is separate from package signing. |
| 2. Install reports invalid manifest/schema | **Home > My games & apps > Add**: full error, element/attribute name and line number if shown; also copy the PC MakeAppx output and `out/payload/AppxManifest.xml`. | Manifest element, capability, namespace or target OS schema is unsupported or invalid. |
| 3. Install reports wrong architecture | **Home > My games & apps > Add**: full error code and package identity/architecture shown there; include `out/payload/AppxManifest.xml`. | Wrong package was selected or package/dependency architecture is incompatible with x64. |
| 4. Install reports a missing dependency | **Home > My games & apps > Add**: full details including dependency name, publisher, minimum version and architecture. | Wrong/stale package or an unavailable framework dependency; this kit currently expects no dependency packages. |
| 5. App launches then exits | `LocalState/ladder-native.log` (even partial); **Home > My games & apps**, selected app: exit/status details; Device Portal **Debug > Crash dumps** and **ETW** output if available, including the original HRESULT/errno diagnostics. | Activation or LocalState/log creation failed, a native API failed, or the process crashed. No log can mean failure before probes started. |
| 6. Black screen | `LocalState/ladder-native.log`, especially n1–n3; Device Portal **ETW** capture if available; note whether the window appeared and whether the expected blue clear ever appeared. | Device/swap-chain creation or Present failed, or the app exited before drawing. A black screen alone does not identify the failed stage. |
| 7. `ladder.log` missing | **File Explorer > Boxedwine app > LocalState**: directory listing and `LocalState/ladder-native.log` if present; if both are absent, copy selected-app status and **ETW** activation diagnostics. | Hosted `ladder.log` is not produced by the console app; wrong app data folder, activation failure or native log creation failure may explain a missing native log. |
| 8. App vs Game mode seems unchanged | `LocalState/ladder-native.log` saved separately after each run as `ladder-App.log` and `ladder-Game.log`, including n5; **Home > My games & apps**, selected app details: capture the app type shown; also record Dev Home **View details > App type**. | Mode was set on a different tile, only the new-app default was changed, or the app was not stopped and relaunched. The normal and Big file opt-in entries need separate settings. |
| 9. Out-of-memory or exit during memory probe | Partial `LocalState/ladder-native.log`, including n5 starting note, reported limit and last successful allocation; Device Portal **Performance > Memory** reading at failure and **Debug > Crash dumps** if available. | The selected mode's memory budget or commit limit was reached; the OS may terminate the process before a final log line. Commit acceptance does not measure physical residency. |
| 10. >2 GB file fails or n7 is SKIP | `LocalState/ladder-native.log`, including activation EntryPoint/opt-in and n7 bytes, size and error codes; **File Explorer > Boxedwine app > LocalState** listing; record available storage reported by Device Portal if shown. | Normal tile intentionally skips n7; opt-in activation may not have arrived, free space may be insufficient, or file I/O failed. The probe uses 2304 MiB and deletes `LocalState/big-file.bin` on close. |

## Platform rungs n10 and n11 (B40)

These two rungs only observe what the console allows. They don't change the
W^X loader or uwph. They run inside `rungs.exe` (after n8, before n5) and write
to `LocalState/ladder-native.log`. Each sub-line starts with `# n10 |` or
`# n11 |`, then `console` (verified Xbox activation) or `standin` (anything
else, e.g. Wine on Linux). A final STAGE line gives the verdict. Off-device the
verdict is always SKIP, even when every stand-in call worked.

n10 platform-allow. Only the codeGeneration path decides PASS/FAIL:

| Sub-line | Meaning |
| --- | --- |
| VirtualAlloc RWX 64KiB ALLOWED/REFUSED ptr= err= | Classic PAGE_EXECUTE_READWRITE allocation (informational). It is freed at once, never written or executed. |
| VirtualFree RWX OK/REFUSED/SKIP | Release of that allocation. |
| VirtualAllocFromApp RW 4KiB OK/REFUSED | Documented codeGeneration path, step 1. |
| write ret 0xC3 OK/SKIP | One `ret` byte written into the RW page. |
| VirtualProtectFromApp RX OK/REFUSED err= | RW to PAGE_EXECUTE_READ. err=127 means the export is missing (desktop Wine). |
| FlushInstructionCache OK/REFUSED/SKIP | Flush before executing. |
| call ret OK returned/SKIP | The single `ret` was called and returned. |
| VirtualFree FromApp OK/REFUSED | Page released. |
| GetModuleHandleA ntdll.dll, then five Nt* names FOUND/MISSING | NtProtectVirtualMemory, NtWaitForSingleObject, NtSetEvent, NtDelayExecution, NtQueryVirtualMemory resolve through GetProcAddress. None are called. |
| STAGE n10 platform-allow PASS/FAIL/SKIP | PASS only on console when `call ret OK returned`. |

n11 loopback-dlls. Only the loopback round-trip decides PASS/FAIL:

| Sub-line | Meaning |
| --- | --- |
| WSAStartup 2.2 OK/REFUSED rc= | Winsock start. |
| listener 127.0.0.1:PORT OK | AF_INET SOCK_STREAM listener with SO_REUSEADDR, bound to INADDR_LOOPBACK port 0; getsockname gave PORT; listen(1). This is the BoxedWine-uwp socketpair sequence. |
| connect+accept OK | A second socket in the same process connected and was accepted. |
| loopback round-trip 4B each way OK / FAIL at STEP; wsa= | `ping` sent one way and `pong` back, with a 2 s receive timeout; STEP names the first call that failed, including receive-timeout setup. |
| socket AF_UNIX SOCK_STREAM ALLOWED/REFUSED wsa= | Whether AF_UNIX sockets exist (informational, closed at once). wsa=10047 is WSAEAFNOSUPPORT. |
| LoadLibraryW FOUND/MISSING | LoadLibraryW/FreeLibrary resolved via GetProcAddress (kernelbase, then kernel32). |
| LoadLibraryW NAME LOADED/REFUSED err= | One line each for d3d11.dll, dxgi.dll, d3d12.dll, xaudio2_9.dll, xinputuap.dll, xinput1_4.dll, mmdevapi.dll, bcrypt.dll, crypt32.dll and ws2_32.dll (informational). Each loaded module is freed straight away. err=126 means not found; err=5 means access denied; err=127 means a required loader export is missing. |
| STAGE n11 loopback-dlls PASS/FAIL/SKIP | PASS only on console when the round-trip is OK. |

`rungs.exe --n10-standin --n11-standin --local-state DIR` runs only these two
rungs off-device (always SKIP). The hosted ladder uses it through
`payload/combo/run_via_wine` and stock desktop Wine in a temporary prefix. That
is our launcher plus Wine's own loader, not uwph and not Xbox proof.

What to send back: the whole `LocalState/ladder-native.log` from each run
(App and Game). Don't trim it. Every `# n10 |` and `# n11 |` line matters,
including REFUSED/MISSING lines and their err/wsa numbers. A REFUSED line is a
result, not a failure to retry another way.

## How to send results back

1. Before each relaunch, download the following through Device Portal **File
   Explorer > Boxedwine app > LocalState**. Keep one folder per run, named by
   date, tile and App/Game mode:
   - `LocalState/ladder-native.log` — required when created; save the normal
     tile's copies as `ladder-App.log` and `ladder-Game.log`, and use separate
     run folders for the Big file opt-in tile.
   - `LocalState/ladder.log` — only if hosted ladder evidence was placed here;
     otherwise report “not produced by console app”.
   - A directory listing of `LocalState` if an expected log is missing. Report
     absent files explicitly. `big-file.bin` is temporary and is not a required
     download; the n7 result in `ladder-native.log` is the evidence to send.
     Keep all `# n10 |` and `# n11 |` sub-lines in it too (REFUSED/MISSING
     with their err/wsa codes are results).
2. If you ran the hosted ladder on Linux, send its `ladder.log` too, labelled
   off-device. A source-tree run (`out/ladder` or `ladder.py`) writes
   `x64/on-device/LocalState/ladder.log`, an extracted kit's `ladder` writes
   `<kit>/LocalState/ladder.log`, and `--local-state DIR` writes
   `DIR/ladder.log`. Include the terminal output of
   `make -C x64/on-device offline-smoke` if you ran it.
3. Download any app crash dump from Device Portal **Debug > Crash dumps** and
   save the relevant **ETW** capture if those facilities are available. Copy
   full install errors from **Home > My games & apps > Add**, or the selected
   app's exit/status details. Preserve numeric codes exactly, without paraphrase.
4. Send a short run note with console model, OS version, package identity,
   tile, App/Game setting, run time, visible result and audible result. Include
   MakeAppx/SignTool output for packaging failures and the public manifest when
   relevant. Label console evidence separately from hosted/off-device results;
   a missing file or SKIP is not a PASS.
5. Review the files before sharing. Omit credentials, tokens, private keys
   (`.pfx`), `LocalState/private/`, and owned game payloads. Review diagnostic
   captures/dumps for private data; send only the relevant reviewed evidence.

## Readiness ladder p1–p6 (B02 O1)

These stages run before n0 in the packaged CoreApplication. No child process,
Wine, or PE loader is needed: p1–p4 and p6 are PE probe sources linked into
`rungs.exe`. Execution requires verified Xbox UWP activation and LocalState.
The hosted ladder emits six SKIPs, including when adapters exist. Build and
offline output are not console evidence.

| Rung | Probe | Native action / current limitation |
| --- | --- | --- |
| p1 | seh_probe | Runs frame-based x64 SEH: AV, divide by zero, illegal instruction, breakpoint, software exception, 10,000 AV recoveries and stack growth. Limits: replaces VEH; guard-page and concurrent VEH checks remain uncovered. |
| p2 | thread_probe | Runs 64 workers, TLS, exact CS/SRW counters, event ping-pong, timer, mutex, semaphore and condition-variable timeout. Validates creation and bounds every wait; closes completed worker handles and retains static resources if a worker times out. |
| p3 | fs_probe | Runs CreateFile2 save/read, case-insensitive reopen, 1MB CreateFileMappingFromApp/MapViewOfFileFromApp mapping, flush/readback, delete and free-space assertions in verified LocalState. Desktop known-folder redirection and install-directory writes remain uncovered. |
| p4 | net_probe | Runs Winsock startup and ephemeral same-process TCP bind/listen/connect/accept/payload assertions, using nonblocking sockets and bounded select waits. External DNS/TLS/WebSocket, fixed-port and cross-app checks remain uncovered; no Steam endpoints or requests. Container denial reports FAIL with socket errors. |
| p5 | jit_probe | SKIP: original VirtualProtect/VirtualAlloc and RWX dual-map test is not rebuilt against app-safe APIs. No original JIT PE executes. A future full port must use VirtualProtectFromApp. |
| p6 | uwph Windows smoke | Runs unchanged `x64/uwp-host/tests/smoke.c` plus unixside and Windows backend in-process, THROUGH-uwph. Creates `smoke-input.txt` in LocalState; full self-test assertions determine exit status. |

Copied readiness sources are under `readiness/`, originally from
`/workspace/xbox-readiness/src/pe`; the SEH/thread/filesystem/network ports are compiled. Originals
remain unchanged. These ports and the uwph self-test compile with
`WINAPI_FAMILY=2` and implicit API declarations rejected. This is a header
check, not Windows SDK certification or console activation proof. Detailed
probe assertions go to stdout; the final exit status goes to ladder-native.log.
Unsupported tests remain explicit SKIPs rather than desktop API fallbacks.

The SEH port uses MinGW's x64 frame scope-table primitive, with ordinary
`__C_specific_handler` dispatch; it installs no vectored handler. App-safe
mapping references: [CreateFileMappingFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingfromapp),
[MapViewOfFileFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffilefromapp).
Frame handling: [Microsoft x64 exception handling](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64).

Desktop regression: `./x64/on-device/readiness/check.sh` builds a separate
fault-instrumented test PE and runs it THROUGH our launcher and Wine's own
loader, with a disposable prefix and a 25-second per-process timeout. It
checks the four ports plus failures creating the first worker, ping-pong
worker, release event and either ping-pong event. Injected failures must
return probe failure without hanging. Production `rungs.exe` contains no
fault wrappers. These are desktop diagnostics, not UWP/console PASS evidence.

## Gamepad rung n8 (B03 O1)

| Rung | Probe | Native action / current limitation |
| --- | --- | --- |
| n8 | Windows.Gaming.Input.Gamepad | Verified Xbox UWP activation only: RoGetActivationFactory, fresh Gamepads vector each second for 10 seconds, count and every mapped XINPUT_STATE logged. Zero controllers is valid; API errors FAIL. Off-device always SKIP. Device read is build-only until console validation. |

`make -C x64/on-device input-test && ./x64/on-device/out/input_test`
checks 28 mapping cases on the host. Buttons map to XInput, unsupported paddles
are ignored, triggers round to 0..255, and sticks round using 32768 for negatives
and 32767 for positives. No deadzone is applied. Inputs are clamped, with
nonfinite values mapped to zero. Packet number is zero: this stateless mapper
does not track changes. No Wine xinput integration is included. The probe uses
the installed SDK-compatible MinGW Gamepad ABI declarations. Host mapping tests
are native C tests, not THROUGH-uwph or stock Wine runtime evidence.

## B21 combo rung

`STAGE combo hello+hello_k32-nofork,noproc,wx` runs the existing B21 hellos
through the packaged launcher, bundled W^X Wine loader, shim and uwph on Linux.
`offline-smoke` requires this rung to PASS, including noproc reports and all
20 ntdll-to-uwph bindings/counters; constraints remain `nofork,noproc,wx`.
`package` and `kit` carry both PE guests plus the Linux runner, launcher, shim,
W^X loader tree and verifiers in `payload/combo/`: `hello.exe`, `hello_k32.exe`,
`run`, `run_via_wine`, `libuwph_wine_shim.so`, `wine-wx/`, `verify_noproc.py`,
`verify_acceptance.py`, `surface.tsv` and `surface-trace.tsv`. MakeAppx packs this directory
too; the console app never runs it. The Linux loader tree retains
B21's links to matching installed Debian Wine 10.0 DLLs/data: these are hosted
diagnostics, not a Windows/UWP runtime. Build-time `prebuilt-prefix` preparation
uses stock Wine. For a source or extracted kit run, set
`UWPH_COMBO_PREFIX=/absolute/path/to/x64/uwp-host/harness/prebuilt-prefix`
(the B21 fixture prepared by `make -C x64/uwp-host/harness prebuilt-prefix`).
Run `UWPH_COMBO_PREFIX=... /path/to/kit/ladder --offline`, or invoke
`/path/to/kit/payload/combo/run` with the same environment. The kit can live
anywhere; it does not require a source checkout. The host must supply Debian
Wine 10.0, Python, bubblewrap and the readiness constraint wrapper plus its
`bin/uwpjail` and `fakesys` support at `/workspace/xbox-readiness`;
`UWPH_UWP_RUN` can select another compatible wrapper. Offline stage a still
requires the source harness; only combo is runnable independently of it.
`offline-smoke` supplies the fixture and checks combo in a relocated kit.
The routed run copies that explicit fixture, disables wineboot,
and retains a separate stock Linux wineserver concession. Logs/reports remain
in `${TMPDIR:-/tmp}/kit-combo.*/`; the temporary prefix is deleted, and no
prefix is packaged.

On device, PASS means both hellos exit 0 through an in-process Wine W^X loader
and uwph with `nofork,noproc,wx`, TEB/thread checks and routing counters verified;
until that native combo adapter exists, report SKIP (the Linux runner cannot run
inside UWP). Hosted online mode accepts `LocalState/adapters/combo` under the
same exit-code/routing contract as the other loader adapters.

n12 `child-process` observes one `CreateProcessW` call to
`Package InstalledLocation\child_probe.exe`, using `GetCurrentPackagePath`.
The silent x64 helper writes `child-ok.txt` to the verified LocalState passed
by the parent and exits 7. The parent clears any stale marker, waits at most
5 seconds, and logs the creation result/GetLastError, wait result, exit code,
and marker existence. ALLOWED requires exit 7 plus the fresh file; REFUSED
records a platform error; INCOMPLETE records a timeout or inconsistent result.
This rung is informational and never retries or uses another launch API.
Unpackaged desktop runs report SKIP; they cannot establish Xbox permission.

## Lifecycle log n13 (informational)

João: during n13's **20 s observation window**, press **Home once**, then
return to the app. Send the entire `LocalState/ladder-native.log` through Device
Portal, with App/Game mode noted. The BEGIN/END lines identify the window;
lifecycle handlers stay registered afterwards and the log stays open. A later
launch replaces the log, so download it before relaunching.

n13 registers ABI-level CoreApplication Suspending, Resuming and
EnteredBackground handlers. Each event appends a UTC timestamp and monotonic
`tick_ms` and flushes the log. Suspending takes a SuspendingDeferral, flushes its
record, immediately calls Complete, and logs the HRESULT and elapsed milliseconds
from obtaining the deferral. It does no artificial background work.

While still in the foreground, n13 requests one ExtendedExecutionSession with
Reason Unspecified and logs ExtendedExecutionResult Allowed/Denied or the
original HRESULT. It pumps the dispatcher during the window, reports a pending
request at the end, cancels it once if needed, and closes the session. Refusals
are observations; there are no alternate APIs or retries. Extended execution
is no guarantee of continued background operation, and missing lifecycle events
are not a PASS or FAIL. This rung is informational. Off-device execution skips
n13 and cannot prove Xbox suspension/resumption behavior.

ABI references: [CoreApplication metadata](https://github.com/microsoft/windows-rs/blob/master/crates/libs/windows/src/Windows/ApplicationModel/Core/mod.rs)
and [ExtendedExecution metadata](https://github.com/microsoft/windows-rs/blob/master/crates/libs/windows/src/Windows/ApplicationModel/ExtendedExecution/mod.rs).
