# Boxedwine Xbox Developer Mode test kit (x64)

Status: no console proof exists yet. The package now starts `rungs.exe`, a native
CoreApplication/IFrameworkView implementation for O5. Its activation, SDK schema
validation and real Xbox API behaviour remain unverified. `smoke.exe` remains a
separate build diagnostic. Tomorrow's checklist:
`/workspace/x64-run/ON-DEVICE-TOMORROW.md`.

## Build the artifacts (Linux)

    make -C x64/on-device package        # loose payload + test cert
    make -C x64/on-device offline-smoke  # package, offline ladder, Wine smoke

- Loose payload: `x64/on-device/out/payload/` (`AppxManifest.xml`, `rungs.exe`, `smoke.exe`, optional `probe_native.exe`,
  `Assets/*.png`, `PACKAGING-NOTES.txt`)
- Test certificate: `x64/on-device/out/test-cert.cer` (CN=BoxedwineDev,
  matches manifest Publisher)
- Private test key: `x64/on-device/out/test-cert.pfx` (empty password; this is
  a disposable dev identity. Never put it in the payload or share it.)

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
sixteen STAGE lines (a–h, n0–n7) and overwrites `LocalState/ladder.log`, flushing each
line. Use `--local-state PATH` to pick another LocalState directory. Each stage
times out after 20 seconds by default (`--timeout 1..60`). A stage FAIL or SKIP
doesn't stop the run, and the exit code stays 0. An unwritable log is an error.
Offline mode runs only stage a, which is the public uwph Linux backend test.
Stages b–h and n0–n7 stay SKIP. This is a hosted Python tool, so it doesn't run on the
console.

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
app from Dev Home. It activates a real CoreWindow and writes
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
