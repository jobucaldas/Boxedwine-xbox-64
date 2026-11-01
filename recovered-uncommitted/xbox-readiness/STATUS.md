# Xbox Steam readiness — STATUS
Updated: 2026-10-24 20:26 BRT (America/Sao_Paulo)
Page: Xbox Steam readiness tracker (WriteCanvas this turn)
Worktree: /workspace/xbox-readiness (isolated from x64-spike driver)

## Headline
- Overall confidence Steam+x64 games will run on Xbox Series Dev Mode UWP: **LOW–MODERATE (~25–35%)**
- Proven off-device: PE load, Steam download+login, Megabonk menu, D3D11/DXVK+D3D12/VKD3D on lavapipe, Steam HTTPS+CM WebSocket, UWP-ish nofork/net/cpu6 jail for hello+5 probes, uwph Linux backend 57/57, Wine ntdll.so binds 5/199 imports to shim.
- Hard walls that need the console (or major host work): stock Wine PE loader uses RWX (R4X patched hello path passes Linux --wx; Xbox untested), wineserver is a separate process, 160 ntdll.so imports unsupported, Vulkan-inside-UWP (dzn blocked on Wine DComp; Xbox has no Vulkan), CoreWindow/input/audio untested on device, 1 GB App vs 5 GB Game RAM budget.

## Battery results (this pass)
| Area | Result | Notes |
|---|---|---|
| UWP nofork | PASS | hello + 5 probes under seccomp (fork/vfork/non-thread clone → EPERM); wineserver pre-started outside jail |
| UWP net (no bind <1024) | PASS | landlock ABI 6; bind 80/1023 fail, 1024+/0 OK; Steam CM WSS works |
| UWP cpu6 | PASS | taskset 0–5 stand-in for Game mode 4+2 cores |
| UWP noproc (no /sys) | PASS nofork+noproc+wx (B21 O2) | run-acceptance.sh (unchanged, default nofork,noproc,wx) rc=0: both hellos exit 0 THROUGH launcher → Wine own loader (bundled W^X ntdll, R4X patch) → shim → uwph with empty /proc, nofork+wx seccomp; fresh routing checks now include mprotect (hello_k32 84 / hello 129 uwph_protect calls, 0 refused RWX) and capped 20/20 trace counters + ntdll bindings pass. Earlier BLOCKED (R4S O4) was the stock RWX ntdll; forcing it (RUN_VIA_WINE_EXACT_LOADER=1) still fails exit 13. No gate weakened. Separate stock Linux wineserver retained; Xbox untested. Video: /workspace/x64-run/videos/B21-O2.mp4 |
| UWP appfs (landlock) | PASS for hello | install dir + /usr ro, LocalState rw |
| UWP W^X (no RWX) | PARTIAL (R4X O4; B21 O2 combo PASS) | B21 O2: full nofork+noproc+wx acceptance passes both hellos; the launcher swaps stock wine64 for the bundled W^X loader under the uwph shim (logged), 0 RWX requests. hello + hello_k32 exit 0 under uwpjail --wx THROUGH launcher → Wine own patched ntdll → shim → uwph; selected bypass=0. Megabonk reaches Unity engine/input, native GameAssembly.dll and MONO environment setup; Mono runtime init unproven. Exact wine-test exits 2: stock controls exit 13; wineboot attempts remain. Linux only, separate stock wineserver; Xbox untested. Video: /workspace/x64-run/videos/R4X-O4.mp4 |
| JIT probe (RW↔RX) | PASS 7/7 | UWP-legal flip pattern + dual-mapping; RWX succeeds on stock Wine (would fail on UWP) |
| SEH/VEH probe | PASS 9/9 | AV, div0, ud2, int3, RaiseException, guard page, 10k faults (~1.8 µs), 4-thread faults |
| Thread/sync probe | PASS 10/10 | 64 threads, TLS, CS+SRW, event ping-pong ~20 µs (wineserver RT), Sleep(16), hires timer |
| FS probe | PASS 7/7 | LocalAppData/AppData/Documents/Temp, case-insensitive, 1 MB MapView |
| Net probe | PASS 7/7 | DNS, HTTPS GetCMList, TLS WebSocket upgrade to cmp2-sea1.steamserver.net:443 |
| BoxedWine x64 suite | 3/4 PASS | t_int/t_misc/t_str PASS; t_fp FAIL (long-double guest vs host) — CPU fidelity, not Xbox blocker (Zen2 runs native) |
| Wine conformance (winetest64) | BLOCKED | daily winetest aborts: unimplemented KERNEL32.ReadConsoleInputExW on this Wine 10.0 |
| SteamAPI_Init | PASS | GameNative steam_api64.dll Init=1 with running client in another prefix |
| Owned games | Megabonk+Hades owned | 771 games; Valheim client not owned (DS was anonymous) |
| D3D11 DXVK / D3D12 VKD3D | PASS | lavapipe; screenshots in videos/04,05 |
| Mesa dzn (Vulkan-on-D3D12) | BUILD PASS / RUN BLOCKED | Mesa 25.1.9 Win64 dzn builds; Wine DCompositionCreateDevice=E_NOTIMPL blocks enumeration. See dzn/RESULT.md |
| R4H uwph surface | 5 original + 20 capped traced imports THROUGH (R4S O4) | Both hellos under nofork+noproc(+wx, B21 O2): all capped O1 entries positive and ntdll bindings target shim. Remaining imports/direct syscalls excluded; +wx acceptance PASS (B21 O2, W^X ntdll). Linux passthrough proof; Win32 backend stock-Wine unit tests are separate, Xbox untested. |
| Megabonk end-to-end | menu via launcher | stock Wine under run_via_wine; NOT through uwph Unix side |

## Videos / proof
- /workspace/xbox-readiness/videos/01-uwp-probes.mp4
- /workspace/xbox-readiness/videos/02-megabonk-stock-wine.mp4 (symlink)
- /workspace/xbox-readiness/videos/03-megabonk-launcher.mp4 (symlink)
- /workspace/xbox-readiness/videos/04-d3d11-dxvk.png
- /workspace/xbox-readiness/videos/05-d3d12-vkd3dp.png
- /workspace/xbox-readiness/videos/06-wine-ntdll-surface.mp4 (R4H-O3)
- /workspace/xbox-readiness/videos/07-uwph-shim-counters.mp4 (R4H-O2)
- /workspace/xbox-readiness/dzn/RESULT.md

## Top on-device-only risks
1. Validate patched RW→RX PE mapping on Xbox with VirtualProtectFromApp/codeGeneration; Linux hello path passes --wx (R4X), full game/UWP acceptance remains partial.
2. wineserver must become in-process; single-process UWP.
3. ~160 ntdll.so POSIX imports + 2 direct syscalls still unbound to uwph.
4. Graphics: no Vulkan on Xbox UWP → need dzn-on-real-D3D12 or D3D12 passthrough; dzn unproven off-device.
5. RAM: App=1 GB vs Game=5 GB — Steam+Wine+Unity game likely needs Game classification.
6. CoreWindow / GameInput / audio / package identity — untested.

## Next (background / later passes)
- Validate R4X PE mapping on Xbox; resolve full wine-test control/wineboot gates without weakening verification.
- Fake /proc provider inside uwph; retest noproc without host /proc.
- Build selected wine tests from /tmp/wine-src (avoid ReadConsoleInputExW runner).
- Hades download + run when disk/time allow (owned).
- Retry dzn with Mesa WSI headless path or DComp stub (research only; still not Xbox proof).

## Plan to 80% (overnight parallel lanes, 2026-10-24 20:47 BRT)
R5 paused. Five parallel worktrees off x64-spike; merge watcher folds DONE lanes back.

| Step | Flips / moves these STATUS rows | Off-device ceiling | On-device only |
|---|---|---|---|
| R4W | nofork concession → real in-process; R4H wineboot gate | Can reach PASS for nofork+hello through uwph | Game-mode process model quirks |
| R4X | W^X FAIL → PASS/PARTIAL; jit under --wx | Can reach PASS for hello/--wx + jit RW→RX | Real VirtualProtectFromApp |
| R4S | uwph surface 5→traced-N; noproc partial→pass | Can clear traced set + fake /proc | Full 160 imports; Xbox syscall ABI |
| R4G | dzn BLOCKED → build/partial | Build+stub/link proof; probe EXE ready | Real Xbox D3D12 / DXIL / present |
| R4D | (new) on-device kit ready | Package+README+offline ladder smoke | Every ladder stage on console |

Honest overnight ceiling if all five DONE: **~55–65%** off-device (sandbox+host path credible; graphics+RAM+input still console). **80% only after** on-device ladder PASSes W^X, in-process server, and at least D3D12 clear — R4D is the vehicle for that tomorrow.
