CC_WIN ?= x86_64-w64-mingw32-gcc
OUT := out
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

.PHONY: all smoke-exe ladder package offline-smoke kit clean
all: smoke-exe

# Loose layout fallback; no SDK packer or device activation is implied.
package: smoke-exe $(OUT)/rungs.exe
	./pack.sh

kit: smoke-exe ladder
	./pack.sh --kit

smoke-exe: $(OUT)/smoke.exe

$(OUT)/smoke.exe: main.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -m64 -mconsole -static -o $@ $<

clean:
	rm -f $(OUT)/smoke.exe $(OUT)/rungs.exe $(OUT)/ladder $(OUT)/run_via_wine

# Hosted Linux diagnostic; device activation remains separate.
ladder: $(OUT)/ladder $(OUT)/rungs.exe

$(OUT)/ladder: ladder.py Makefile
	mkdir -p $(OUT)
	cp ladder.py $@
	chmod +x $@

# Linux-box smoke: package, offline ladder, payload exe via run_via_wine -> Wine.
offline-smoke: ladder
	./offline-smoke.sh

$(OUT)/rungs.exe: rungs.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -Wno-missing-field-initializers -m64 -mconsole -static -o $@ $< $(UWPH_OBJS) $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(OUT)/readiness/thread.o -lbcrypt -lsynchronization -lwindowsapp -lole32 -lruntimeobject -luuid -ldxguid -lm

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
$(OUT)/readiness/thread.o: readiness/thread_probe.c readiness/common.h
	mkdir -p $(@D)
	$(CC_WIN) $(READINESS_FLAGS) -Dmain=readiness_thread_main -c $< -o $@
$(OUT)/rungs.exe: $(UWPH_OBJS) $(OUT)/readiness/unixside.o $(OUT)/readiness/smoke.o $(OUT)/readiness/thread.o
