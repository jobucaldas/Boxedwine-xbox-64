CC ?= gcc
OUT := out
CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC -pthread -DUWPH_BACKEND_LINUX_PASSTHROUGH -I../include
SOURCES := $(wildcard ../src/*.c) linux_backend.c
HEADERS := $(wildcard ../include/uwph/*.h ../src/*.h)
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
vpath %.c ../src .
$(OUT)/%.o: %.c $(HEADERS) counters.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/libuwph.a: $(OBJECTS)
	ar rcs $@ $^
$(OUT)/backend_test: backend_test.c $(OUT)/libuwph.a
	$(CC) $(CFLAGS) $< $(OUT)/libuwph.a -o $@
backend-test: $(OUT)/backend_test
	timeout 15s $(OUT)/backend_test
.PHONY: backend-test

# O2 traced set on the passthrough archive: real host calls, no shim/Wine.
$(OUT)/traced: ../tests/traced.c $(OUT)/libuwph.a
	$(CC) $(CFLAGS) $< $(OUT)/libuwph.a -ldl -o $@
traced-test: $(OUT)/traced
	timeout 15s $(OUT)/traced "$(abspath ../tests)"
.PHONY: traced-test

$(OUT)/shim.map: shim.map
	@mkdir -p $(OUT)
	cp $< $@
$(OUT)/libuwph_shim.so: shim.c counters.c counters.h $(OUT)/libuwph.a $(OUT)/shim.map
	$(CC) $(CFLAGS) -shared shim.c counters.c $(OUT)/libuwph.a -ldl -Wl,--version-script=$(OUT)/shim.map -Wl,-z,defs -o $@
$(OUT)/elf_probe: elf_probe.c
	$(CC) $(CFLAGS) -fno-builtin $< -ldl -o $@
shim-test: $(OUT)/libuwph_shim.so $(OUT)/elf_probe
	@rm -f $(OUT)/negative.report $(OUT)/positive.report $(OUT)/exit.report $(OUT)/fixture.created
	@printf 'hello from O2 fixture\n' > $(OUT)/fixture
	UWPH_REPORT=$(CURDIR)/$(OUT)/negative.report timeout 15s $(OUT)/elf_probe $(OUT)/fixture
	timeout 15s env UWPH_REPORT=$(CURDIR)/$(OUT)/positive.report LD_PRELOAD=$(CURDIR)/$(OUT)/libuwph_shim.so $(OUT)/elf_probe $(OUT)/fixture
	timeout 15s env UWPH_REPORT=$(CURDIR)/$(OUT)/exit.report LD_PRELOAD=$(CURDIR)/$(OUT)/libuwph_shim.so $(OUT)/elf_probe $(OUT)/fixture --exit
	python3 verify_counters.py $(OUT)/negative.report $(OUT)/positive.report $(OUT)/exit.report
.PHONY: shim-test

# Host-only inventory and relocation diagnostic; no Wine execution proof.
wine-surface-check: out/libuwph_shim.so out/run_hello
	./prepare-wine.sh
	./check_surface.sh
.PHONY: wine-surface-check

# Separate backend object: native fd interoperability is exclusive to Wine proof.
$(OUT)/wine_linux_backend.o: linux_backend.c counters.h $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DUWPH_HARNESS_NATIVE_FDS -c $< -o $@
$(OUT)/libuwph_wine_shim.so: inproc_server.c ../wineserver/server_thread.c ../wineserver/out/libwineserver.a shim.c counters.c counters.h $(OUT)/wine_linux_backend.o $(OUT)/libuwph.a $(OUT)/shim.map
	$(CC) $(CFLAGS) -DUWPH_HARNESS_NATIVE_FDS -shared inproc_server.c ../wineserver/server_thread.c ../wineserver/out/libwineserver.a shim.c counters.c $(OUT)/wine_linux_backend.o $(OUT)/libuwph.a -ldl -Wl,--version-script=$(OUT)/shim.map -Wl,-z,defs -o $@
# Bundles the private W^X loader: used instead of stock wine64 under the shim.
$(OUT)/run_via_wine: ../../pe/launcher/run_via_wine.c $(OUT)/wine-wx/.stamp
	$(CC) -O2 -Wall -Wextra -Werror -DRUN_VIA_WINE_WX_LOADER='"$(abspath $(OUT))/wine-wx/lib/wine/wine64"' $< -o $@
wine-test: $(OUT)/libuwph_wine_shim.so $(OUT)/run_via_wine $(OUT)/wine-wx/.stamp
	./run-wine.sh
.PHONY: wine-test

# Desktop PE -> uwph Windows waits/signals -> stock Wine Win32 implementation.
# Distinct from wine-test, which routes real Unix ntdll.so calls via Linux uwph.
windows-futex-test:
	./run-windows-smoke.sh --futex-only
windows-smoke-test:
	./run-windows-smoke.sh --full
.PHONY: windows-futex-test windows-smoke-test

windows-file-test:
	./run-windows-smoke.sh --file-only
.PHONY: windows-file-test

# Bounded live backend check plus explicitly labelled saved longer-run evidence.
evidence-replay: $(OUT)/backend_test
	./evidence-replay.sh
.PHONY: evidence-replay

# O3 provider assertions run with host /proc hidden (not stock Wine proof).
$(OUT)/fake_proc_test: ../tests/fake_proc.c $(OUT)/libuwph.a
	$(CC) $(CFLAGS) $< $(OUT)/libuwph.a -ldl -o $@
noproc-test:
	./run-noproc.sh
.PHONY: noproc-test

# O4 strict acceptance: wx is included by default when supported by uwpjail.
acceptance-test:
	./run-acceptance.sh
.PHONY: acceptance-test
# R4X O2: private W^X ntdll.so (RW map/relocate, then per-section RX/R/RW).
$(OUT)/wine-wx/.stamp: prepare-wine-wx.sh wine-image-wx.patch
	./prepare-wine-wx.sh
$(OUT)/run_hello: run-hello.sh $(OUT)/libuwph_wine_shim.so $(OUT)/run_via_wine $(OUT)/wine-wx/.stamp
	cp $< $@
	chmod +x $@

# R4X O3: readiness jit_probe.exe under --wx (RW->RX pass, RWX refused).
# install-readiness-glue writes /workspace/xbox-readiness/bin/jit_probe, a
# one-line exec of this checkout's out/run_jit_probe (the O3 exact test path).
$(OUT)/run_jit_probe: run-jit-probe.sh $(OUT)/libuwph_wine_shim.so $(OUT)/run_via_wine $(OUT)/wine-wx/.stamp
	cp $< $@
	chmod +x $@
READINESS_JIT := /workspace/xbox-readiness/bin/jit_probe
READINESS_PE := /workspace/xbox-readiness/src/pe
MINGW_CC ?= x86_64-w64-mingw32-gcc
$(READINESS_JIT).exe: $(READINESS_PE)/jit_probe.c $(READINESS_PE)/common.h
	$(MINGW_CC) -O2 -Wall -Wextra $< -o $@
install-readiness-glue: $(OUT)/run_jit_probe $(READINESS_JIT).exe
	printf '#!/bin/sh\n# R4X O3 glue (installed by x64/uwp-host/harness make install-readiness-glue)\nexec %s "$$@"\n' '$(CURDIR)/$(OUT)/run_jit_probe' > $(READINESS_JIT)
	chmod +x $(READINESS_JIT)
jit-test: install-readiness-glue
	/workspace/xbox-readiness/bin/uwpjail --wx -- $(READINESS_JIT)
.PHONY: install-readiness-glue jit-test

$(OUT)/protect_test: protect_test.c $(OUT)/libuwph.a
	$(CC) $(CFLAGS) $< $(OUT)/libuwph.a -o $@
protect-test: $(OUT)/protect_test
	/workspace/xbox-readiness/bin/uwpjail --wx -- $<
.PHONY: protect-test

# Explicit packaging-time initialization only; never a runtime prerequisite.
prebuilt-prefix:
	./prepare-wine.sh --prebuilt-prefix
.PHONY: prebuilt-prefix

../wineserver/out/libwineserver.a: FORCE
	$(MAKE) -C ../wineserver lib
$(OUT)/libuwph_inproc_server.so: inproc_server.c ../wineserver/server_thread.c ../wineserver/out/libwineserver.a
	$(CC) $(CFLAGS) -shared $^ -Wl,-z,defs -o $@
wine-test-inproc-build: $(OUT)/libuwph_wine_shim.so $(OUT)/libuwph_inproc_server.so $(OUT)/run_via_wine
wine-test-inproc: wine-test-inproc-build
	./run-wine.sh --inproc-server
FORCE:
.PHONY: wine-test-inproc wine-test-inproc-build FORCE
