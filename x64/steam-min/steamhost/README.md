# Steamhost metadata watcher

Run `bash x64/steam-min/steamhost/osw-watch.sh` from any directory. Requires
Python 3 with HTTPS trust roots. It reports OSW HEAD, the build from its win64
manifest (win32 fallback), the last change under `OpenSteamworks/Generated`,
the bins archive's HTTP HEAD status, the local pin, and Valve's current version.
All OSW reads use the reported revision; at most five anonymous GitHub requests
are made. Valve manifests are GETs; bins packages are HEAD only, never downloaded.

`match-current` means the published table build equals Valve's current build and
its bins archive returns 200. `match-old-only` means the archive returns 200 but
the versions differ; `none` means the archive returns 404. These are availability
checks, not validation of interface compatibility or permission to change PIN.txt.
Exit is 0 for a completed check (including 404), 2 for network/HTTP failures or
unusable metadata. No login, credentials, Wine, or UWP execution is involved.

Pinned historical fetch (no client execution):

```sh
bash x64/steam-min/steamhost/fetch-pin.sh --pin x64/steam-min/steamhost/PIN-1788652215.txt
```

Run from the repository root. This checks the historical manifest version and
`bins_win64` URL/hash against the pin, verifies the downloaded archive SHA256,
then extracts unchanged bytes to `out/client-1788652215/`. The DLL hash must
match before the staged tree replaces the destination. Cached packages are
rehashed on every use; a corrupt cache is downloaded again. CEF/webhelper and
unsafe archive paths are refused by the shared Valve fetcher. At least 10 GiB
free is required before downloading. `out/` is ignored; no Valve binaries or
interface maps belong in commits. This command provides fetch-only evidence,
not a Wine, uwp-host, or console runtime test.
