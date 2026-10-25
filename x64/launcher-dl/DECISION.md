Decision: use a small GPL-2.0 C++ content client, with a replaceable host TLS transport; O1 builds its desktop Windows x64 transport and tests it under stock Wine.

This is an anonymous downloader, not a Steam client engine or a DRM service.
It does not load, replace, patch, or execute any downloaded Valve binary.
O2 account authentication/QR is outside this change.

| Candidate | Licence with this GPL-2 tree | UWP / .NET Native / C++/WinRT | TLS | Size |
| --- | --- | --- | --- | --- |
| SteamKit2 + DepotDownloader | SteamKit2 declares LGPL-2.1-only; DepotDownloader uses GPL-2.0. GPL-2 distribution is possible subject to library licence/source and relinking requirements; preserve third party notices. | Current projects target modern .NET (SteamKit2 net8.0/net10.0; DepotDownloader net9.0). These are not UWP .NET Native targets. A desktop self-contained or NativeAOT executable does not establish Xbox UWP support. Retargeting needs an API, protobuf/reflection/AOT, transport and filesystem audit. | Managed HTTP/WebSocket delegates TLS to the desktop runtime/OS. A UWP port must use an available UWP stack, with ordinary certificate verification. | Framework dependent output requires a separately installed runtime; self-contained output bundles it. Not built/measured here; no numerical size claim. Much broader protocol coverage than this prototype. |
| Small C++ client (chosen) | New client GPL-2.0-only; existing repo zlib uses its permissive licence; LZMA decoder public domain; Zstandard decoder BSD-3-Clause option. No SteamKit code linked. | Portable protobuf/VDF/manifest logic avoids CLR/JIT/reflection. A C++/WinRT host can implement the transport interface using documented Windows.Web.Http.HttpClient and Windows.Networking.Sockets.MessageWebSocket. This adapter and app-local storage integration are still required; O1 is **not** an AppContainer/UWP binary or a console proof. | Desktop O1 uses WinHTTP with normal certificate checks (Wine Schannel/GnuTLS off device). WinHTTP is not the proposed UWP transport. Console plan is host WinRT TLS, internetClient capability, no Wine guest TLS requirement. Never suppress certificate errors. | Static MinGW x64 C++ executable measured by build.sh; 2,877,989 bytes in the final O1 build, including decompression and C++ runtime, no CLR. Build output records exact bytes. Console package size unmeasured. |

Why: the native protocol core has a direct C++/WinRT integration path without
requiring a modern CLR in an Xbox UWP process. The tradeoff is maintenance:
this prototype deliberately implements only anonymous app 1007, public branch,
Windows-compatible depots, bounded files/chunks, SHA-1 and current ZIP/VZa/VSZa
formats. It fails on unsupported content, denied access, malformed messages,
unsafe paths, wrong hashes and empty selection. Prefer SteamKit2 for a normal
desktop launcher; its maturity does not remove the UWP runtime mismatch here.

## Reproduce and interpret

Run `bash x64/launcher-dl/test-anon.sh`. It cross-builds a PE32+ x86-64 executable,
uses a dedicated stock Wine prefix and a fresh output directory each time, logs
on anonymously over CM WebSocket TLS on 443, requests current PICS metadata,
depot keys and manifest access codes, and downloads manifests and chunks only
from `https://steampipe.akamaized.net`. There is no token-file input or account
login path. Manifest/download files and a TSV of expected file SHA-1/size are
retained below the reported fresh run directory.

Every selected regular file is reconstructed from bounded, contiguous chunks,
checked against each chunk SHA-1, checked against manifest file SHA-1 and size,
then reopened and hashed from disk. Only then can `VERIFIED app=1007` appear.
Manifest identity (depot and GID), ZIP CRC and Valve compression CRC are checked.
Manifest signatures are not verified; manifest SHA-1 values come over host TLS.
This is transport/content integrity verification, not an independent signature
attestation. No preexisting SDK redist files count as downloaded proof.

O1 explicitly permits **stock Wine** for this test. This does not test
THROUGH-uwph, Wine inside the UWP launcher, AppContainer networking, Xbox TLS,
.NET Native, or on-console execution. O1 is not console PASS.

## Primary sources checked 2026-10-25

* [DepotDownloader project](https://github.com/SteamRE/DepotDownloader/blob/master/DepotDownloader/DepotDownloader.csproj) and [GPL-2 licence](https://github.com/SteamRE/DepotDownloader/blob/master/LICENSE).
* [SteamKit2 target frameworks, dependency and licence declarations](https://github.com/SteamRE/SteamKit/blob/master/SteamKit2/SteamKit2/SteamKit2.csproj).
* [Microsoft MessageWebSocket UWP API and capabilities](https://learn.microsoft.com/en-us/uwp/api/windows.networking.sockets.messagewebsocket).
* [Microsoft Windows.Web.Http.HttpClient](https://learn.microsoft.com/en-us/uwp/api/windows.web.http.httpclient).
* [Steam protocol protobuf definitions](https://github.com/SteamDatabase/Protobufs/tree/master/steam), also inspected in the existing steam-user package. No private Steam client DLL interfaces used.
* [Zstandard v1.5.7 decoder amalgamation](https://github.com/facebook/zstd/tree/v1.5.7/build/single_file_libs) and [BSD licence](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE).
* [LZMA SDK public-domain licence](https://www.7-zip.org/sdk.html); provenance in third_party/lzma/README.md.
