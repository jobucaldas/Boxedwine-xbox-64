# R4G O1: dzn DirectComposition blocker

O1 evidence/documentation check: PASS by citing retained logs, as explicitly
allowed by the order. Runtime remains BLOCKED. No new runtime execution.
Source report: `/workspace/xbox-readiness/dzn/RESULT.md` (2026-10-24).
Copied raw evidence: `evidence/R4G-O1/`; filenames below refer there.

## Exact command and cited output

Working directory: `/workspace/xbox-readiness/dzn/runtime`.
The original `probe-run.sh` entrypoint executes:

```sh
WINEPREFIX=/workspace/xbox-readiness/dzn/pfx \
WINEDEBUG=-all DISPLAY=:93 WINEDLLOVERRIDES='d3d12,d3d12core=n' \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
VKD3D_DEBUG=info timeout 120s wine ./dzn_probe.exe
```

`probe-dxil.log`, recorded process exit 10 (`status.log`):

```text
create(&ci,NULL,&inst): 0
MESA: error: Failed to create DXCore adapter factory
vkEnumeratePhysicalDevices(inst,&count,NULL): -3
```

The source report's isolated API test, working directory
`/workspace/xbox-readiness/dzn`:

```sh
x86_64-w64-mingw32-gcc -O2 -Wall dcomp_probe.c -o runtime/dcomp_probe.exe
WINEPREFIX="$PWD/pfx" WINEDEBUG=-all DISPLAY=:93 timeout 60s wine runtime/dcomp_probe.exe
```

`dcomp-probe.log`, recorded process exit 3 (`status.log`):

```text
DCompositionCreateDevice(NULL, IID_IDCompositionDevice): HRESULT=0x80004001 device=0000000000000000
```

`diagnostic.log:235` also records Wine's `fixme:dcomp:DCompositionCreateDevice`
at the dzn failure. `loader-evidence.log:12` identifies the backend as
`llvmpipe (LLVM 19.1.7, 256 bits)`.

## R4G O1

All Mesa file:line references below are relative to
`/workspace/xbox-readiness/dzn/src/mesa-25.1.9/` and were checked against that
retained source tree.

The blocking call is `src/vulkan/wsi/wsi_common_win32.cpp:954`:
`DCompositionCreateDevice(NULL, IID_PPV_ARGS(&device))`. E_NOTIMPL
(0x80004001) makes `dcomp_get_device()` return NULL at line 956.
`wsi_win32_init_wsi()` obtains it at line 988 and maps NULL to
`VK_ERROR_INITIALIZATION_FAILED` at line 992. Device enumeration reaches
this even without a surface: `src/microsoft/vulkan/dzn_device.c:1164`
calls `dzn_wsi_init()`, whose Windows branch sets `sw=false` at
`src/microsoft/vulkan/dzn_wsi.c:91` and passes it at line 101.
The earlier DXCore failure has a DXGI fallback; it is not the final wall.

Additional WSI pieces required after device creation, identified by source
inspection, not measured failures (all in `src/vulkan/wsi/wsi_common_win32.cpp`):

| Required piece | Site | Evidence limit |
|---|---|---|
| DXGI composition swapchain and interface | 795–796 | Next swapchain dependency; unexecuted |
| Composition target for HWND | 802 | Requires an HWND presentation path; unexecuted |
| Visual, root, swapchain content, commit | 806–809 | Returning S_OK alone from device creation cannot supply these objects |
| Backbuffer resource access | 431 | GetBuffer not reached |
| Presentation | 691 | Present1 not reached |
| Window extent | 179 | GetClientRect uses desktop HWND; CoreWindow adaptation unimplemented by this order |

A documented UWP/CoreWindow presentation path remains to be designed and
validated; this inspected Mesa path uses composition plus HWND. O1 implements
no DComp stub or CoreWindow bridge. These are follow-on gaps, not proof that
Microsoft's Xbox implementation lacks those APIs.

## Proof classification

CITED STOCK WINE: desktop Wine 10.0 loads the PE probe and dzn ICD, with
VKD3D-Proton over Linux lavapipe. THROUGH-uwph/our launcher: NOT EXERCISED.
Build-only: no new build; original build success is reported by the source
report. The O1-specific permission to cite logs is used here; the evidence
is not promoted to a through-host runtime proof. No device, compute dispatch,
presentation, Xbox GPU, UWP capability, or console compatibility proof.
Lavapipe is software and is not Xbox D3D12. No video was recorded because
this order cites existing textual failure evidence and performs no visual run.
