# Native graphics TEB boundary

Use guest COM wrappers whose methods enter a host callback through
`uwph_host_call`; do not hand a raw host COM interface to guest code for direct
method calls. A shared x64 calling convention does not establish host TEB state.
The boundary test observes the fake guest PEB address `0x12345000` on a raw
GS read without dereferencing it. That demonstrates state exposure, not an
intentional crash or proof that every possible direct API call fails.

`tests/teb_boundary.c` links our Windows-target uwph static library. It saves
host state, uses the existing `uwph_teb_enter_guest` five-field swap with fake
PEB/static TLS values, and installs distinct values in all 64 inline TLS slots
and LastError. The exported callback helper restores the saved host PEB, static
TLS pointer, inline TLS slots, and LastError before calling the callback. The
callback checks those fields, allocates/frees memory with
`HeapAlloc(GetProcessHeap())`, reads an allocated TLS slot with `TlsGetValue`,
and round-trips `SetLastError`/`GetLastError`. After return the test verifies
all fake guest fields and restores host state before printing or exiting.

The helper is a bounded supplement to the existing swap, not a complete runtime
dispatcher: its caller supplies a same-thread host snapshot taken before guest
entry, and callbacks must return normally. It does not replace the existing
StackBase/StackLimit/Self swap; this experiment keeps those fields at host
values. TLS expansion slots, exception unwinding across the helper, arbitrary
reentrant guest callbacks, and other TEB fields require additional integration
before general graphics use. Host LastError changes are consumed inside the
callback (or returned explicitly in its argument/result); guest LastError is
preserved. Persistent host TLS mutations would likewise need explicit snapshot
management. Do not infer that the existing five-field dispatcher now gains
these additional fields automatically.

Validation command:

```
make -C x64/native-gfx teb-test && WINEDEBUG=-all timeout 60 wine x64/native-gfx/out/teb_boundary.exe
```

Expected output includes `TEB BOUNDARY PASS`. This is THROUGH-uwph boundary
coverage, executed by Wine's PE loader, not a stock-Wine-only API test. Off-device
host system APIs are Wine stand-ins. This does not validate the Xbox system DLLs,
UWP packaging, D3D11/COM behavior, or console performance. No visual output is
produced, so no video is needed for this order.

Off-device validation passed with the command above. The required
`make -C x64/uwp-host test` reached the Windows smoke invocation but failed
because `wine64` is absent from PATH; use the authorized `WINE=wine` override
for that invocation without removing the check.
That retry reports 560 passed and two failed fixture checks: its default root
`x64\\uwp-host\\tests` is relative to the working directory changed by `make -C`.
Running the same Windows smoke executable from the repository root passes
(566 passed, 0 failed). The required make invocation therefore remains blocked
by the existing fixture-path issue; no unrelated test files were changed.
