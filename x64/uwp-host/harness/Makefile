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

$(OUT)/shim.map:
	@mkdir -p $(OUT)
	printf '{ global: open; read; close; mmap; munmap; _exit; uwph_shim_report; local: *; };\n' > $@
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
wine-surface-check: out/libuwph_shim.so
	./prepare-wine.sh
	./check_surface.sh
.PHONY: wine-surface-check

# Separate backend object: native fd interoperability is exclusive to Wine proof.
$(OUT)/wine_linux_backend.o: linux_backend.c counters.h $(HEADERS)
	$(CC) $(CFLAGS) -DUWPH_HARNESS_NATIVE_FDS -c $< -o $@
$(OUT)/libuwph_wine_shim.so: shim.c counters.c counters.h $(OUT)/wine_linux_backend.o $(OUT)/libuwph.a $(OUT)/shim.map
	$(CC) $(CFLAGS) -DUWPH_HARNESS_NATIVE_FDS -shared shim.c counters.c $(OUT)/wine_linux_backend.o $(OUT)/libuwph.a -ldl -Wl,--version-script=$(OUT)/shim.map -Wl,-z,defs -o $@
$(OUT)/run_via_wine: ../../pe/launcher/run_via_wine.c
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
wine-test: $(OUT)/libuwph_wine_shim.so $(OUT)/run_via_wine
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
