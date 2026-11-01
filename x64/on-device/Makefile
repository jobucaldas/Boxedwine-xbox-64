CC_WIN ?= x86_64-w64-mingw32-gcc
OUT := out
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

.PHONY: all smoke-exe ladder package verify-package offline-smoke kit clean linux-appx linux-appx-test
all: smoke-exe

# Loose layout fallback; no SDK packer or device activation is implied.
package: smoke-exe $(OUT)/rungs.exe $(OUT)/probe_d3d12.exe combo-binaries
	./pack.sh

# B60: signed .appx on Linux (makemsix + osslsigncode), then offline checks.
linux-appx: package
	linux/pack-linux.sh

linux-appx-test: linux-appx
	linux/test-linux-appx.sh

kit: smoke-exe ladder $(OUT)/probe_d3d12.exe combo-binaries
	./pack.sh --kit

# An explicit PACKAGE_DIR checks an existing bundle without rebuilding it.
PACKAGE_DIR ?= $(OUT)/kit
ifeq ($(origin PACKAGE_DIR),file)
verify-package: kit
endif
verify-package:
	./verify-package.sh "$(PACKAGE_DIR)"

smoke-exe: $(OUT)/smoke.exe

$(OUT)/smoke.exe: main.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -m64 -mconsole -static -o $@ $<

clean:
	rm -rf $(OUT)

# Hosted Linux diagnostic; device activation remains separate.
ladder: $(OUT)/ladder $(OUT)/rungs.exe $(OUT)/probe_d3d12.exe

$(OUT)/ladder: ladder.py Makefile
	mkdir -p $(OUT)
	cp ladder.py $@
	chmod +x $@

# R4G standalone D3D12 probe, same flags as x64/graphics; d3d12/dxgi load at runtime.
$(OUT)/probe_d3d12.exe: ../graphics/probe_d3d12.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -municode -static-libgcc -s -o $@ $< -ldxguid

# Linux-box smoke: package, offline ladder, payload exe via run_via_wine -> Wine.
offline-smoke: ladder
	./offline-smoke.sh

$(OUT)/rungs.exe: rungs.c logview.inc logfont.h input/mapping.c input/mapping.h input/probe.inc Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -Wno-missing-field-initializers -m64 $(UWP_EXE_FLAGS) -static -o $@ $< input/mapping.c $(UWPH_OBJS) $(OUT)/readiness/elf_load.o $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(READINESS_OBJS) $(OUT)/readiness/vp_shim.o $(OUT)/readiness/libntdll_seh.a -lws2_32 -lbcrypt -lsynchronization -lwindowsapp -lole32 -lruntimeobject -luuid -ldxguid -lm
	python3 linux/pe-appcontainer.py $@

# B61: Xbox activation contract (uwp-crossbuild gotchas 18-22): GUI subsystem >= 6.02,
# UCRT app CRT instead of msvcrt.dll, AppContainer bit set post-link. Else 0x8027025B.
# linux/vp_shim.c routes the CRT's VirtualProtect to VirtualProtectFromApp (no KERNEL32 import).
UCRT_DEFS := -D_UCRT -D__MSVCRT_VERSION__=0xE00
UWP_EXE_FLAGS := -m64 -mwindows -mcrtdll=ucrtapp $(UCRT_DEFS) -Wl,--major-subsystem-version,6,--minor-subsystem-version,2

READINESS_OBJS := $(addprefix $(OUT)/readiness/,thread.o seh.o fs.o net.o)

# In-process PE readiness module and the unchanged public uwph Windows self-test.
APP_DEFS := -DWINAPI_FAMILY=2 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008
UWPH_ROOT := ../uwp-host
UWPH_SRCS := $(wildcard $(UWPH_ROOT)/src/*.c)
UWPH_OBJS := $(patsubst $(UWPH_ROOT)/src/%.c,$(OUT)/readiness/uwph-%.o,$(UWPH_SRCS))
READINESS_FLAGS := -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration $(APP_DEFS) $(UCRT_DEFS)
$(OUT)/readiness/uwph-%.o: $(UWPH_ROOT)/src/%.c
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -I$(UWPH_ROOT)/include -c $< -o $@
$(OUT)/readiness/unixside.o: $(UWPH_ROOT)/tests/unixside.c
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -I$(UWPH_ROOT)/include -DWINE_UNIX_LIB -c $< -o $@
$(OUT)/readiness/smoke.o: $(UWPH_ROOT)/tests/smoke.c
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -I$(UWPH_ROOT)/include -I$(UWPH_ROOT)/src -Dmain=readiness_uwph_main -c $< -o $@
$(OUT)/readiness/%.o: readiness/%_probe.c readiness/common.h
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -Dmain=readiness_$*_main -c $< -o $@
# B61: mingw's static ucrtapp __C_specific_handler did not catch the p1 AV on the console
# (unhandled 0xC0000005 dump); take the OS one from ntdll, as msvcrt.dll did before.
$(OUT)/readiness/libntdll_seh.a: linux/ntdll-seh.def
	mkdir -p $(OUT)/readiness
	x86_64-w64-mingw32-dlltool -d $< -l $@ -D ntdll.dll

$(OUT)/readiness/vp_shim.o: linux/vp_shim.c Makefile
	mkdir -p $(OUT)/readiness
	$(CC_WIN) $(READINESS_FLAGS) -c $< -o $@

$(OUT)/rungs.exe: $(OUT)/readiness/libntdll_seh.a $(OUT)/readiness/vp_shim.o $(UWPH_OBJS) $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(READINESS_OBJS)

# Optional off-device regression PE; production objects have no fault injection.
$(OUT)/readiness/thread-check.o: readiness/thread_probe.c readiness/common.h
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -Dmain=readiness_thread_main -DCreateThread=probe_CreateThread -DCreateEventExW=probe_CreateEventExW -c $< -o $@
$(OUT)/readiness-check.exe: readiness/check.c $(OUT)/readiness/thread-check.o $(filter-out $(OUT)/readiness/thread.o,$(READINESS_OBJS))
	$(CC_WIN) $(READINESS_FLAGS) -static -o $@ $^ -lws2_32 -lwindowsapp -lsynchronization

.PHONY: input-test
input-test: $(OUT)/input_test $(OUT)/ladder

$(OUT)/input_test: input/input_test.c input/mapping.c input/mapping.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ input/input_test.c input/mapping.c -lm

# Existing B21 guests and routed Linux runtime; no new probes.
.PHONY: combo-binaries
combo-binaries:
	mkdir -p $(OUT)/combo-guests
	$(CC_WIN) -O1 -nostdlib -e entry -o $(OUT)/combo-guests/hello_k32.exe ../pe/tests/hello_k32.c -lkernel32
	$(CC_WIN) -O1 -o $(OUT)/combo-guests/hello.exe ../pe/tests/hello.c
	$(MAKE) -C ../uwp-host/harness out/libuwph_wine_shim.so out/run_via_wine prebuilt-prefix

WINE_ELF_DIR ?= /workspace/Boxedwine-xbox-64/x64/uwp-host/harness/out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix
$(OUT)/readiness/elf_load.o: elf_load.c elf_load.h elf_adapters.inc
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -Wno-misleading-indentation -I$(UWPH_ROOT)/include -c $< -o $@
$(OUT)/rungs.exe: $(OUT)/readiness/elf_load.o
$(OUT)/elf-test.exe: elf_test.c elf_wine_compat.c $(OUT)/readiness/elf_load.o $(UWPH_OBJS)
	$(CC_WIN) $(READINESS_FLAGS) -I$(UWPH_ROOT)/include -mcrtdll=ucrtapp -o $@ $^ -lwindowsapp -lws2_32 -lsynchronization -lm
.PHONY: elf-test
elf-test: $(OUT)/elf-test.exe
	WINEDEBUG=-all wine $(OUT)/elf-test.exe 'Z:$(WINE_ELF_DIR)'
