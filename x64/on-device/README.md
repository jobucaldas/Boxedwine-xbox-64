# Boxedwine Xbox Developer Mode test kit (x64)

Status: no console proof exists yet. Everything below has run only on a Linux
box. `smoke.exe` is a console diagnostic, not the UWP activation bridge
(`Boxedwine.App` is still pending), so on the console the install may work but
launching may fail or exit at once. Write down what actually happens; don't
read it as success. Tomorrow's checklist:
`/workspace/x64-run/ON-DEVICE-TOMORROW.md`.

## Build the artifacts (Linux)

    make -C x64/on-device package        # loose payload + test cert
    make -C x64/on-device offline-smoke  # package, offline ladder, Wine smoke

- Loose payload: `x64/on-device/out/payload/` (`AppxManifest.xml`, `smoke.exe`,
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
4. In Dev Home (or the app's details in Device Portal), set the app type of
   **Boxedwine Dev Mode** to **Game**. This gets the Game/Title memory budget.
   Check the budget on the device; we haven't measured it.
5. Launch it from Dev Home. Record whether it activates, crashes, or exits. Also
   record any error code and the Device Portal crash/ETW output.
6. To remove it: **My games & apps**, then pick the app and **Remove**.

Out of scope: Store submission, real Microsoft signing, and any security,
hypervisor or code-signing bypass. Use documented capabilities only.

## Manifest and capabilities (O1)

`AppxManifest.xml` targets Windows.Xbox/x64 and declares `codeGeneration`
(base foundation capability), `internetClient` and `privateNetworkClientServer`.
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
eight STAGE lines (a–h) and overwrites `LocalState/ladder.log`, flushing each
line. Use `--local-state PATH` to pick another LocalState directory. Each stage
times out after 20 seconds by default (`--timeout 1..60`). A stage FAIL or SKIP
doesn't stop the run, and the exit code stays 0. An unwritable log is an error.
Offline mode runs only stage a, which is the public uwph Linux backend test.
Stages b–h stay SKIP. This is a hosted Python tool, so it doesn't run on the
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
