# x64/native-gfx — R4N native D3D11/DXGI passthrough

Lane R4N work (plan: `/workspace/x64-run/lanes/R4N/ORDERS.md`). Everything for the
native-API fast path lives under this directory; `x64/graphics/` (R4G) and
`x64/on-device/` (R4D) are not touched from here.

## probe_native.exe (O1)

A single PE x64 console EXE that asks the system's own graphics, audio and
device-info APIs what they can do and prints one `NATIVE <family> ...` line for
each. R4D runs it on the console as one rung of its ladder.

```
make -C x64/native-gfx probe                      # -> x64/native-gfx/out/probe_native.exe
wine x64/native-gfx/out/probe_native.exe --log /tmp/probe.log
```

`--log <path>` mirrors every line to a file. The default is `probe_native.log` in the cwd.
The exit code is always 0. When an API fails you get `status=FAIL` with its hr; when
it is missing you get `status=SKIP`. Lines are flushed one at a time and the cheap
probes run first, so if a driver crashes the log still holds every line before it.

| line             | what it calls                                                                 |
|------------------|-------------------------------------------------------------------------------|
| `NATIVE host`    | `ntdll!wine_get_version` present → `os=wine-<ver> note=stand-in`; else `os=windows` |
| `NATIVE memory`  | `GlobalMemoryStatusEx` → `avail_mb`, `total_mb`, `load`                        |
| `NATIVE gaming`  | `GetGamingDeviceModelInformation` via `GetProcAddress` on `api-ms-win-gaming-deviceinformation-l1-1-0.dll`; `SKIP` if the DLL or export is missing; decodes Xbox One/One S/One X/Series S/Series X ids |
| `NATIVE dxgi`    | `CreateDXGIFactory2` + `QueryInterface(IID_IDXGIFactory2)`                     |
| `NATIVE d3d11`   | `D3D11CreateDevice(NULL, HARDWARE, levels 11_1..10_0)` (retries without 11_1 on `E_INVALIDARG`) → `fl`, adapter description/vendor/device/VRAM, `warp=1` if the adapter is software or is Basic Render (0x1414/0x8c) |
| `NATIVE d3d12`   | `D3D12CreateDevice(NULL, FL 11_0, IID_ID3D12Device)`                           |
| `NATIVE xaudio2` | `LoadLibraryW("xaudio2_9.dll")` + `GetProcAddress("XAudio2Create")`            |

The `dll=builtin|native` field shows whether the loaded module is a Wine builtin PE,
detected by the "Wine builtin DLL" marker after the DOS header.

### Off-device results are a stand-in

Under Wine, every line describes **Wine** (wined3d, Wine's dxgi/d3d12/xaudio2), not
the Xbox. `NATIVE host` says `note=stand-in` in that case. Reference run on the dev
box (Wine 10.0, no GPU):

```
NATIVE host os=wine-10.0 note=stand-in (off-device; results describe Wine, not the Xbox)
NATIVE memory status=OK avail_mb=12395 total_mb=16002 load=22%
NATIVE gaming status=SKIP reason=no-export
NATIVE dxgi status=OK hr=0x0 qi_factory2_hr=0x0 dll=builtin
NATIVE d3d11 status=OK hr=0x0 fl=0xb100 adapter="NVIDIA GeForce GTX 470" vendor=0x10de device=0x06cd vram_mb=16002 warp=0 dll=builtin
NATIVE d3d12 status=OK hr=0x0 fl=0xb000 dll=builtin
NATIVE xaudio2 status=OK hr=0x0 dll=builtin
```

The "GTX 470" adapter is the name wined3d invents for the GL renderer it finds. It is
not real hardware. Nobody has run this on an Xbox yet, so there are no Xbox results here.

The local gaming-info declaration follows the [Windows SDK structure documentation](https://learn.microsoft.com/windows/win32/api/gamingdeviceinformation/ns-gamingdeviceinformation-gaming_device_model_information).
The Series model IDs are documented in [DirectX and UWP on Xbox Series X/S](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/).
The off-device test cannot exercise this export; console validation remains pending.
