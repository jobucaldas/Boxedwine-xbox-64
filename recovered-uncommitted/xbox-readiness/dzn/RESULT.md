# Mesa dzn Windows x64 feasibility test — Linux/Wine stand-in

**Result: build PASS; runtime BLOCKED at Wine DirectComposition during physical-device enumeration.**

Run: 2026-10-24, approximately 23:10–23:16 UTC. Stopped at the first identified hard wall, well within the 45-minute build-attempt / 75-minute total budgets. All source, build, prefix, runtime files and logs are under `/workspace/xbox-readiness/dzn`. No console was accessed; no changes were made to `/workspace/Boxedwine-xbox-64`; nothing was pushed or posted.

## Outcome

| Stage | Result | Evidence |
|---|---|---|
| Fetch Mesa release and DirectX-Headers | PASS | `logs/mesa-download.log`, `logs/headers-fetch.log` |
| MinGW cross configuration | PASS after installing PyYAML | `logs/configure.log` (exit 1), `logs/configure2.log` (exit 0) |
| Mesa dzn + SPIR-V-to-DXIL build, `-j3` | PASS, exit 0 | `logs/build.log`, all 510 steps |
| Install DLL and ICD JSON | PASS, exit 0 | `logs/install.log` |
| Build Windows C probe and compute SPIR-V | PASS, exit 0 | `logs/probe-compute-build.log`, `logs/shader-build.log` |
| Direct-load ICD without DXIL.dll | FAIL: `vkCreateInstance` = -3 | `logs/probe-default.log`, process exit 10 |
| Direct-load ICD with `DZN_DEBUG=experimental`, no DXIL.dll | Instance PASS; enumeration FAIL | `logs/probe-experimental.log`, process exit 10; experimental shader-model enabling rejected |
| Direct-load ICD with official DXIL.dll, default dzn mode | Instance PASS; enumeration FAIL | `logs/probe-dxil.log`, process exit 10 |
| Confirm actual Linux Vulkan backend | PASS: lavapipe (`llvmpipe (LLVM 19.1.7, 256 bits)`) | `logs/loader-evidence.log` |
| Isolate blocking Wine API | `DCompositionCreateDevice` = `0x80004001` (`E_NOTIMPL`), NULL device | `logs/dcomp-probe.log`, process exit 3 |
| Vulkan device creation / compute readback / presentation | NOT REACHED | Physical-device enumeration failed first |
| Stretch DXVK D3D11 probe | SKIPPED | Base dzn enumeration is blocked; winevulkan routing was not attempted |

## Exact versions and inputs

- Mesa **25.1.9**, release tarball: <https://archive.mesa3d.org/mesa-25.1.9.tar.xz>.
- DirectX-Headers **1.619.5**, Git commit **461ef5138bd30922b44fbad77c4e47662812d2d5**, shallow clone of <https://github.com/microsoft/DirectX-Headers>. Used as Mesa's local fallback subproject through `src/mesa-25.1.9/subprojects/DirectX-Headers-1.0 -> ../../DirectX-Headers`. Mesa's wrap originally names tag v1.614.1; the local checkout satisfies Mesa's >=1.614.1 requirement. No Mesa source patches.
- MinGW GCC/G++ **14-win32**, linker binutils **2.44**; MinGW Windows headers package **12.0.0-5**. Native GCC **14.2.0**.
- Local Python venv: Python **3.13.5**, Meson **1.12.1**, Ninja **1.13.2**, Mako **1.4.3**, MarkupSafe **3.0.4**, packaging **26.3**, PyYAML **6.0.3**.
- Wine **10.0**, Debian package **10.0~repack-6**; fresh 64-bit prefix `pfx/`. Existing X server socket `:93` was reused; no Xvfb was started or stopped.
- VKD3D-Proton **3.0.1**, runtime reports build **3b10bd7a7ec6a73**. Copied only the user-specified `d3d12.dll` and `d3d12core.dll` from `/workspace/r3-dl/vkd3d-proton-3.0.1/x64/` into `runtime/`.
- Host lavapipe: `mesa-vulkan-drivers` **25.0.7-2+deb13u1**, LLVM **19.1.7**. Linux Vulkan loader package **1.4.309.0-1**. Its ICD JSON advertises API **1.4.305**.
- glslang-tools **15.1.0+1.4.309.0-1**; bison **3.8.2**, flex **2.6.4**.
- DXIL: official DirectXShaderCompiler release **v1.9.2609**, asset **dxc_2026_09_29.zip** from <https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609>. Extracted `bin\x64\dxil.dll` to `runtime/dxil.dll`. No dxcompiler.dll installed. Release API response retained in `downloads/dxc-release.json`.
- SHA-256 hashes: `logs/artifact-sha256.txt`, `logs/download-sha256.txt`; environment details: `logs/environment.log`.

## Commands and reproduction

Working directory for these commands is `/workspace/xbox-readiness/dzn`. `run.sh LABEL COMMAND...` records shell-escaped arguments to `logs/commands.log`, raw combined stdout/stderr to `logs/LABEL.log`, and the actual command's exit status to `logs/status.log` (it preserves pipeline status).

Dependency installation used passwordless sudo and pip:

```sh
sudo -n apt-get install -y meson ninja-build python3-mako python3-packaging bison flex glslang-tools pkg-config mingw-w64
python3 -m venv venv
venv/bin/pip install meson ninja mako packaging
venv/bin/pip install pyyaml
curl -fL https://archive.mesa3d.org/mesa-25.1.9.tar.xz -o downloads/mesa-25.1.9.tar.xz
tar -xf downloads/mesa-25.1.9.tar.xz -C src
git clone --depth 1 https://github.com/microsoft/DirectX-Headers.git src/DirectX-Headers
ln -s ../../DirectX-Headers src/mesa-25.1.9/subprojects/DirectX-Headers-1.0
```

All dependency installation/download commands completed with exit 0. The first Meson configure returned exit 1 for missing PyYAML in the venv; installing it resolved the only configuration failure. apt also installed system Meson 1.7.0 / Ninja 1.12.1; the build explicitly selected the venv versions through PATH.

Cross file is retained verbatim as `cross-win64.ini`: Windows/x86_64/little endian, `x86_64-w64-mingw32-{gcc,g++,ar,strip,windres}`, pkg-config, `needs_exe_wrapper=true`, isolated target pkg-config directory `install/lib/pkgconfig`.

```sh
export PATH="$PWD/venv/bin:$PATH"
meson setup build src/mesa-25.1.9 --cross-file cross-win64.ini \
  --prefix "$PWD/install" --buildtype=release \
  -Dvulkan-drivers=microsoft-experimental -Dgallium-drivers= \
  -Dplatforms=windows -Dbuild-tests=false -Dspirv-to-dxil=true \
  -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= \
  -Dzstd=disabled -Dzlib=disabled -DDirectX-Headers:build-test=false
ninja -C build -j3
meson install -C build --no-rebuild
x86_64-w64-mingw32-gcc -O2 -Wall -Isrc/mesa-25.1.9/include dzn_probe.c -o runtime/dzn_probe.exe
glslangValidator -V compute.comp -o runtime/compute.spv
WINEPREFIX="$PWD/pfx" WINEARCH=win64 WINEDEBUG=-all DISPLAY=:93 wineboot -u
```

Installed artifacts:

- `install/bin/vulkan_dzn.dll` (~17 MB), also copied to `runtime/vulkan_dzn.dll`.
- `install/share/vulkan/icd.d/dzn_icd.x86_64.json`: API **1.1.311**, relative DLL path `..\..\..\bin\vulkan_dzn.dll`.
- `install/bin/libspirv_to_dxil.dll`, `install/bin/spirv2dxil.exe`, static/import libraries under `install/lib/`.
- MinGW `libgcc_s_seh-1.dll` and `libstdc++-6.dll` copied into `runtime/` as precaution; dzn itself does not import them (`logs/dzn-imports.txt`).

`probe-run.sh` is the exact runtime entrypoint. It changes to `runtime/`, uses a 120-second timeout and runs:

```sh
WINEPREFIX=/workspace/xbox-readiness/dzn/pfx \
WINEDEBUG=-all DISPLAY=:93 WINEDLLOVERRIDES='d3d12,d3d12core=n' \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
VKD3D_DEBUG=info timeout 120s wine ./dzn_probe.exe
```

Before DXIL installation: `./run.sh probe-default ./probe-run.sh`, then `./run.sh probe-experimental env DZN_DEBUG=experimental ./probe-run.sh`. Downloaded DXC using `curl -fL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip -o downloads/dxc_2026_09_29.zip`; extracted only the x64 DXIL DLL using Python zipfile. Then ran `./run.sh probe-dxil ./probe-run.sh` and `./run.sh loader-evidence env VK_LOADER_DEBUG=driver ./probe-run.sh`.

`diagnostic-run.sh` repeats the final test with `WINEDEBUG=+loaddll,+debugstr`, `VKD3D_DEBUG=trace`, and `MESA_VK_DEBUG=errors`; this intentionally overrides the normal quiet logging solely for diagnosis. `logs/diagnostic.log` confirms native DXIL/d3d12/d3d12core loading and Wine's builtin DComp stub. An earlier `probe-default-trace` attempt was ineffective because `probe-run.sh` hardcodes WINEDEBUG=-all; that log is retained but is not used as loader evidence.

The isolated API reproducer is `dcomp_probe.c`:

```sh
x86_64-w64-mingw32-gcc -O2 -Wall dcomp_probe.c -o runtime/dcomp_probe.exe
WINEPREFIX="$PWD/pfx" WINEDEBUG=-all DISPLAY=:93 timeout 60s wine runtime/dcomp_probe.exe
```

It exits 3 and prints `HRESULT=0x80004001 device=0000000000000000`.

## Exact hard wall

With official DXIL present, `vkCreateInstance` succeeds (0). Enumeration calls Mesa's DXCore path first, which logs `Failed to create DXCore adapter factory`. This is **not the final blocker**: `dzn_enumerate_physical_devices()` falls back to DXGI on Windows (`src/microsoft/vulkan/dzn_device.c`, around line 1741).

The fallback reaches D3D12 device creation through VKD3D-Proton; its initialization logs show Vulkan device setup, memory topology, bindless support, and capabilities. Linux loader diagnostics restrict discovery to `lvp_icd.json` / `libvulkan_lvp.so` and show the sole underlying device as `llvmpipe (LLVM 19.1.7, 256 bits)`.

Then `dzn_physical_device_create()` calls `dzn_wsi_init()` even though the probe creates no surface. On Windows this supplies `sw_device=false` (`src/microsoft/vulkan/dzn_wsi.c`, lines 86–104). `wsi_win32_init_wsi()` unconditionally obtains an IDCompositionDevice in that mode (`src/vulkan/wsi/wsi_common_win32.cpp`, around lines 961–995). `dcomp_get_device()` calls `DCompositionCreateDevice(NULL, IID_IDCompositionDevice, ...)`; Wine returns E_NOTIMPL. Mesa maps failure to `VK_ERROR_INITIALIZATION_FAILED` (-3), propagating back through `vkEnumeratePhysicalDevices`. The diagnostic log and separate API reproducer agree.

Stopped here. No Wine stub replacement, Mesa WSI patch, driver feature spoofing, or alternative no-WSI rebuild was attempted. Therefore no physical-device properties/features could be printed through the public dzn Vulkan API, and no Vulkan device or compute pipeline was created.

## DXIL behavior

Mesa's Windows code explicitly loads `DXIL.dll`, creates `IDxcValidator`, and fails instance creation if either step fails (`src/microsoft/compiler/dxil_validator.cpp`, around lines 171–196; `src/microsoft/vulkan/dzn_device.c`, around lines 1823–1834). Default testing without the DLL returned -3; adding the official DLL changed instance creation to success. Native loading is confirmed in `logs/diagnostic.log`.

`DZN_DEBUG=experimental` skips that validator requirement, but dzn then requests `D3D12ExperimentalShaderModels`. On this VKD3D-Proton setup it reports `failed to enable experimental shader models` and enumeration returns -3. Providing DXIL is the working dependency resolution for instance creation here. **No shader was compiled or submitted at runtime**, so actual validator signing and acceptance of Mesa-generated DXIL remain untested. dxcompiler.dll is optional diagnostic support in this source and was not needed to reach the WSI wall.

## Probe coverage

`dzn_probe.c` uses `LoadLibraryA("vulkan_dzn.dll")` and `vk_icdGetInstanceProcAddr`, avoiding a separate Khronos Windows loader as permitted by the task. It requests Vulkan 1.1, enumerates devices, and is written to report device name/API and geometry/tessellation/int64/float64/sparse, buffer-device-address, descriptor-indexing and timeline features. It then creates a compute-capable device. `compute.inc` contains descriptor, pipeline, coherent host buffer, dispatch, barrier, wait, and 64-word readback checks. `compute.comp` writes `0x12340000 + invocation index`. Both probe and shader compile successfully, but the compute path is **unexecuted and unvalidated** because enumeration fails first.

## What this does NOT prove

- This is a desktop Windows x64 cross-build running under Linux Wine, **not a UWP/AppContainer build**, deployment, certification, or console execution test.
- VKD3D-Proton is not Microsoft's D3D12 implementation. Successful internal D3D12 setup on lavapipe does not establish Xbox behavior, supported interfaces, performance, or correctness.
- Xbox D3D12 Feature Level 11_0 / Shader Model up to 6.4 restrictions were not imposed or tested. Mesa requests a baseline FL11_0 device but queries higher capabilities. Source inspection shows buffer-device-address and descriptor-indexing support gated on SM >=6.6; this is a relevant limitation to investigate for the stated SM6.4 ceiling, not a measured Xbox result.
- No shader signing on a real D3D12 device, allowed UWP DXIL validator deployment, or console shader acceptance was tested. No inference is made that experimental shader models are available on Xbox.
- No Vulkan conformance, game rendering, image clear/readback, presentation, synchronization stress, graphics pipeline, DXVK, VKD3D-Proton-on-dzn, Wine winevulkan bridge, or end-to-end Wine game compatibility was demonstrated. dzn prints its own non-conformant/testing-only warning.
- The Wine DirectComposition wall is a limitation of this stand-in and does not prove an equivalent failure on Microsoft's OS. Conversely, bypassing it in a later headless experiment would not validate UWP presentation APIs or documented Xbox capabilities.
- The ICD JSON was generated/installed and inspected; registration and discovery by a Khronos Windows Vulkan loader were not tested. Direct ICD loading was used.

All command exit statuses are retained in `logs/status.log`; raw logs, Meson configuration evidence, sources, generated binaries and the fresh prefix remain available for follow-up.
