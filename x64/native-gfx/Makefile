# x64/native-gfx - R4N native D3D11/DXGI passthrough work (see README.md).
#
#   make probe     build out/probe_native.exe (on-device native-API probe for the R4D ladder)
#   make run-probe build + run it under wine (off-device: results describe Wine = stand-in)

OUT    ?= out
CC_WIN ?= x86_64-w64-mingw32-gcc
WINE   ?= wine
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-cast-function-type

THUNK_SOURCES = d3dpass/host.c d3dpass/guest.c generated/host.c generated/guest.c
THUNK_HEADERS = d3dpass/d3dpass.h d3dpass/runtime.h generated/packets.h


.PHONY: all probe run-probe clean

all: probe

probe: $(OUT)/probe_native.exe

$(OUT)/probe_native.exe: probe_native.c
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) -o $@ $< -ld3d11 -ldxgi -ld3d12 -lole32 -luuid

run-probe: probe
	WINEDEBUG=-all $(WINE) $(OUT)/probe_native.exe --log $(OUT)/probe_native.log

clean:
	rm -rf $(OUT)

.PHONY: teb-test
teb-test: $(OUT)/teb_boundary.exe

../uwp-host/out/win/libuwph.a: FORCE
	$(MAKE) -C ../uwp-host out/win/libuwph.a

.PHONY: FORCE
FORCE:

$(OUT)/teb_boundary.exe: tests/teb_boundary.c ../uwp-host/out/win/libuwph.a
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) -I../uwp-host/include -o $@ $< ../uwp-host/out/win/libuwph.a -lbcrypt -lsynchronization -lwindowsapp

.PHONY: clear-test
clear-test: $(OUT)/clear_readback.exe

$(OUT)/clear_readback.exe: tests/clear_readback.c $(THUNK_SOURCES) $(THUNK_HEADERS) ../uwp-host/out/win/libuwph.a
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) -I../uwp-host/include -Id3dpass -o $@ tests/clear_readback.c $(THUNK_SOURCES) ../uwp-host/out/win/libuwph.a -ld3d11 -ldxgi -luuid -lbcrypt -lsynchronization -lwindowsapp

.PHONY: gen check
gen:
	python3 tools/gen_com_thunks.py

$(OUT)/bench_thunk.exe: tests/bench_thunk.c $(THUNK_SOURCES) $(THUNK_HEADERS) ../uwp-host/out/win/libuwph.a
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS) -I../uwp-host/include -Id3dpass -o $@ tests/bench_thunk.c $(THUNK_SOURCES) ../uwp-host/out/win/libuwph.a -ld3d11 -ldxgi -luuid -lbcrypt -lsynchronization -lwindowsapp

check: clear-test $(OUT)/bench_thunk.exe
	python3 tools/gen_com_thunks.py --check
	WINEDEBUG=-all timeout 90 $(WINE) $(OUT)/clear_readback.exe
	WINEDEBUG=-all timeout 90 $(WINE) $(OUT)/bench_thunk.exe
