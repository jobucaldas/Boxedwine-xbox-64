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
