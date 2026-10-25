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
