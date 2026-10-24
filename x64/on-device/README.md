# Xbox Developer Mode x64 skeleton (O1)

`make -C x64/on-device smoke-exe` builds `out/smoke.exe` with
`x86_64-w64-mingw32-gcc`. It is a runnable x64 Windows console status program.
This order is build-only: no stock Wine, uwph, Wine loader, or on-console
execution is claimed. The program does not load game code.

`AppxManifest.xml` is an XML skeleton targeting Windows.Xbox and x64. It declares
`codeGeneration` (base foundation capability), `internetClient`, and
`privateNetworkClientServer`. Capability declarations alone do not prove
availability or authorization on a particular console OS.

The manifest's `Boxedwine.App` entry point reserves the future UWP activation
bridge; the console `main` does not implement that bridge. Referenced Assets
are also pending. This directory is not yet an installable UWP package. XML
well-formedness and a MinGW build do not establish SDK schema validation,
package activation, or compatibility with the UWP API surface.

For Game/Title memory, select the Game application type in Xbox Developer Home
when a deployable title exists. The available budget must be checked on-device;
this desktop smoke does not measure or reserve title memory.

Intended runtime integration uses our uwp-host/launcher and Wine's own PE loader
in-process, with documented UWP capabilities. O1 supplies no loader integration,
signing, Store submission, package, or deployment automation.

O2 hosted ladder: `make -C x64/on-device ladder &&
./x64/on-device/out/ladder --offline`. Requires Python 3 and the existing
Linux uwph harness toolchain. Prints all eight STAGE results and overwrites
`x64/on-device/LocalState/ladder.log`, flushing each result. `--local-state PATH`
selects a host-provided application LocalState directory. Infrastructure errors
(e.g. unwritable log) are errors; stage FAIL/SKIP results do not stop collection
or change the final zero exit. Each stage has a 20 second timeout (configurable
with `--timeout 1..60`). Offline runs only the existing public uwph Linux backend
test; the other stages remain SKIP. This is a hosted Python tool, not UWP app
activation, a console run, or a PE loader proof.

LocalState layout (generated/private files are ignored by Git):

- `ladder.log`: status and fixed diagnostic reasons only.
- `adapters/a` through `adapters/h`: optional executable integration adapters.
- `private/steam.token`: optional user-provided token; never shipped, opened,
  printed, or passed on argv by the ladder. Missing file skips g.
- `payload/Megabonk.exe`: optional owned payload; missing file skips h.

Without `--offline`, an installed adapter runs with LocalState as its working
directory, stdout/stderr discarded, and no shell. Adapters return 0 only for
verified success, 77 for unavailable prerequisites, any other code for FAIL.
No adapters are supplied yet: their absence is SKIP, never PASS. Adapters must
use public uwph for a and d (RW allocation -> RX transition -> execute/verify,
never RWX); our launcher and Wine's own loader for b/c/f/g/h; verify an actual
in-process server for e; and verify the dzn adapter and D3D12 device for f.
Steam g must test the real SteamAPI_Init result without emulation or DRM bypass.
An executable starting successfully alone cannot establish these assertions.
Adapter return codes are trusted diagnostic inputs, not independent routing
verification. The current separate Linux wineserver harness cannot satisfy e.
Offline mode disables these adapters, even when installed. No token or payload
is fetched, and no stock Wine fallback exists in this runner.
