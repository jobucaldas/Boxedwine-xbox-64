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

## Generated COM boundary and call measurement (O4)

```
make -C x64/native-gfx gen check
```

`tools/gen_com_thunks.py` reads MinGW's `d3d11.h`, `d3d11_1.h`, `dxgi.h`,
`dxgi1_2.h`, and `unknwn.h`. It emits checked-in `generated/guest.c`,
`generated/host.c`, and `generated/packets.h`. `--include DIR` selects another
MinGW header tree. There are no downloaded or manually copied vtable signatures.
The generator covers the twelve requested interfaces plus their typed interface
dependencies (46 interfaces total). It excludes unrelated video/crypto APIs.
Each generated vtable slot has a typed function; each host stub calls the typed
host vtable with the host TEB restored. AddRef/Release also use generated stubs;
the wrapper pool tracks adopted references separately from driver references.

`check` compares every generated file with a fresh generation, prints the
requested generated/header method counts and checks their exact slot order.
Compiled static assertions also compare vtable sizes against the installed
headers. It builds and runs the unchanged O3 clear/readback test and the new
benchmark through the linked uwph registry and `uwph_host_call`. The benchmark
also checks Device1/Context1, IID-directed outputs, and vertex-buffer interface
array round trips before measuring.

`bench_thunk.exe` warms both paths, then measures 1,000,000 calls to
`IASetPrimitiveTopology` per path on the same device context. Topology alternates
between line and triangle lists. The thunk loop uses synthetic guest PEB/TLS and
the generated boundary; the direct baseline runs only with restored host state.
QueryPerformanceCounter runs in host state outside both loops. The thunk timing
includes one guest-entry/exit pair around the loop. The test verifies exactly
1,000,000 measured boundary crossings, the final topology, and restored guest
fields. It prints `THUNK_NS=<n> DIRECT_NS=<m>` in nanoseconds per call. These are
**off-device Wine builtin stand-in measurements, never Xbox timings**.

This remains a same-thread prototype with a bounded 512-wrapper pool. Interface
arrays use explicit count/direction rules because C vtable declarations omit
those semantics; unknown patterns abort generation. Arrays above 4096 entries
(including special preserve-binding sentinel counts) fail closed rather than
passing wrapper addresses to the driver. IID outputs outside the generated
interface set return E_NOINTERFACE. Exhaustion is reported as E_OUTOFMEMORY for
HRESULT-returning methods; void methods can return NULL, and callers must check.
Opaque private-data buffers, external COM objects/callbacks, reentrancy,
thread-safe identity/lifetime handling, and on-device swapchain/handle adaptation
still need integration before general game use. Typed-pointer generation alone
does not establish those semantics. Swapchain methods are emitted but their
CoreWindow adaptation belongs to O5 and is not tested here. No Xbox DLL or UWP
package was exercised.
