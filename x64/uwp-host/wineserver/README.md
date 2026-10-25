# libwineserver.a: Wine 10.0's server as a static library

This is a build-only step toward running wineserver inside the UWP host
process. It compiles Wine 10.0's own `server/*.c` with no source changes and
packs the objects into `out/libwineserver.a`. Wine's `main()` is renamed to
`wine_server_main()` with `objcopy --redefine-sym` so that a host can link the
archive alongside its own `main`. Nothing in this directory runs the server.

    make lib       # configure /workspace/wine-build once, then build out/libwineserver.a
    make clean     # remove out/ and the server objects

Inputs (overridable): `WINE_SRC=/tmp/wine-src` (Debian 10.0~repack source),
`WINE_BUILD=/workspace/wine-build` (out-of-tree configure, never committed).

- `build.sh configure` runs `configure --no-create --enable-win64 --without-x
  --without-freetype --without-mingw ...` and then `config.status
  include/config.h`. The server needs only `config.h`. A full `config.status`
  run goes on to makedep over the whole tree, and that fails on the Debian
  repack because the binary test resources are stripped
  (`dlls/amstream/tests/rsrc.rc: test.avi`).
- The Makefile takes the source list from Wine's `server/Makefile.in`. It uses
  CC/CFLAGS/EXTRACFLAGS from configure, plus the defines makedep gives
  `server/`: `-D__WINESRC__ -DWINE_UNIX_LIB`, and for `unicode.c` the
  `BINDIR`/`DATADIR` from `unicode_EXTRADEFS`. The one addition is `-fPIC`,
  so the archive can go into a PIE or shared host.
- `out/protocol_version` records `SERVER_PROTOCOL_VERSION`. `build.sh prepare`
  regenerates `wine/server_protocol.h`, `request_handlers.h` and
  `request_trace.h` with Wine's own `tools/make_requests`, as the Debian
  repack does, so the version is 1 rather than upstream 10.0's 855. This is
  the version clients check, and it matches the installed Debian
  `wine64 10.0~repack-6` ntdll.so and wineserver64.

Every undefined symbol in the archive is a libc symbol, so the archive links
against libc alone. That list includes process-wide calls (`fork`, `setsid`,
`chdir`/`fchdir`, `atexit`, `exit`, `sigaction`, `ptrace`), which still need a
thread-hosting shim before the server can share a process with the Wine
client (see NOTES-x64.md "R4W O3").
