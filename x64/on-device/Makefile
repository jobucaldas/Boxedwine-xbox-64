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

$(OUT)/rungs.exe: rungs.c input/mapping.c input/mapping.h input/probe.inc Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -Wno-missing-field-initializers -m64 -mconsole -static -o $@ $< input/mapping.c $(UWPH_OBJS) $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(READINESS_OBJS) -lws2_32 -lbcrypt -lsynchronization -lwindowsapp -lole32 -lruntimeobject -luuid -ldxguid -lm

READINESS_OBJS := $(addprefix $(OUT)/readiness/,thread.o seh.o fs.o net.o)

# In-process PE readiness module and the unchanged public uwph Windows self-test.
APP_DEFS := -DWINAPI_FAMILY=2 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008
UWPH_ROOT := ../uwp-host
UWPH_SRCS := $(wildcard $(UWPH_ROOT)/src/*.c)
UWPH_OBJS := $(patsubst $(UWPH_ROOT)/src/%.c,$(OUT)/readiness/uwph-%.o,$(UWPH_SRCS))
READINESS_FLAGS := -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration $(APP_DEFS)
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
$(OUT)/rungs.exe: $(UWPH_OBJS) $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(READINESS_OBJS)

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
