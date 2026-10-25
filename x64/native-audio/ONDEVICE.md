# XAudio2 2.9 probe kit

PE executable: `x64/native-audio/out/xa2_probe.exe` (build with
`make -C x64/native-audio`). Worktree artifact:
`/workspace/wt/B04/x64/native-audio/out/xa2_probe.exe`.

The probe resolves `XAudio2Create` from `xaudio2_9.dll` in-process, then
creates a mono mastering voice and PCM source voice, queues one second of
48 kHz / 16-bit 440 Hz sine, and waits for 48,000 consumed samples. A timeout,
failed HRESULT, or incomplete buffer returns failure without a played marker.

For the console kit, use the same native forwarder pattern as the R4N d3d11
forwarder: resolve the console-provided xaudio2_9.dll using the host's permitted
packaged/system-library loading path and forward XAudio2Create to that native
module in the same process. Voice interfaces then call the native implementation;
there is no audio translation or cross-process component. This probe's desktop
LoadLibraryW call is not an implemented UWP loader integration. Availability,
exports, packaging permissions, and interface compatibility must be checked on
the actual Developer Mode kit using documented capabilities. Do not copy a
console system DLL into this repository or bypass platform loading policy.

On desktop Wine, normal DLL resolution supplies Wine's builtin xaudio2_9
(FAudio); no native override or replacement DLL is bundled. Off-device success
is stock-Wine proof only, not THROUGH-uwph proof and not evidence of console
audio. Run the PE through our launcher/Wine loader integration on the kit as a
separate future check; no on-device result is claimed here.

## B04 off-device result

Passed on Linux stock Wine with a dedicated PulseAudio `module-null-sink`
(`b04_null`, 48 kHz stereo), `PULSE_SERVER=unix:/tmp/B04-pulse/native`,
`WINEPREFIX=/tmp/B04-xa2-prefix`, and a free Xvfb display. SDL dummy alone
was insufficient on this Wine installation. The final exact command was:

```sh
make -C x64/native-audio && WINEDEBUG=-all timeout 60 wine x64/native-audio/out/xa2_probe.exe | tee /tmp/xa2.log; grep -q 'XA2 played_ms=' /tmp/xa2.log
```

Output: `XA2 create hr=0x00000000`, `XA2 voices ok`,
`XA2 played_ms=1000 samples=48000`. This verifies buffer consumption by the
builtin implementation into a null sink, not audible output. Video:
`/workspace/x64-run/videos/B04-O1.mp4`.
