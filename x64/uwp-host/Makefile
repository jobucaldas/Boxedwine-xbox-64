# uwph - POSIX host surface for Wine's Unix side in a Dev Mode UWP app.
#
#   make            build both backends + tests, run the wine64 surface check
#   make test       also run the smoke tests (Linux natively, Windows build under wine64)
#
# Linux build  = stub backend (host calls -> ENOSYS), for layout/ABI checks.
# Windows build = mingw-w64 with WINAPI_FAMILY_APP headers: only APIs that the
#                 SDK headers expose to UWP apps compile (desktop-only ones are
#                 resolved at runtime and probed, see include/uwph/probe.h).

OUT      ?= out
SRCS     := $(wildcard src/*.c)
HDRS     := $(wildcard include/uwph/*.h) $(wildcard src/*.h)
CFLAGS_COMMON := -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude

CC_LINUX ?= gcc
CC_WIN   ?= x86_64-w64-mingw32-gcc
WINE     ?= $(shell command -v wine64 2>/dev/null || printf /usr/lib/wine/wine64)
# WINAPI_FAMILY_APP = 2; Windows 10 2004 headers level
WIN_DEFS := -DWINAPI_FAMILY=2 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008

LIN_OBJS := $(SRCS:src/%.c=$(OUT)/linux/%.o)
WIN_OBJS := $(SRCS:src/%.c=$(OUT)/win/%.o)

all: $(OUT)/linux/libuwph.a $(OUT)/linux/smoke $(OUT)/linux/abi_check $(OUT)/linux/traced \
     $(OUT)/win/libuwph.a $(OUT)/win/smoke.exe $(OUT)/win/traced.exe check

$(OUT)/linux/%.o: src/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC_LINUX) $(CFLAGS_COMMON) -fPIC -pthread -c -o $@ $<

$(OUT)/win/%.o: src/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_COMMON) $(WIN_DEFS) -c -o $@ $<

$(OUT)/linux/libuwph.a: $(LIN_OBJS)
	ar rcs $@ $^

$(OUT)/win/libuwph.a: $(WIN_OBJS)
	x86_64-w64-mingw32-ar rcs $@ $^

# ABI check: uwph's Linux constants/layouts vs glibc (compile-time asserts)
$(OUT)/linux/abi_check: tests/abi_check.c $(HDRS)
	@mkdir -p $(@D)
	$(CC_LINUX) $(CFLAGS_COMMON) -o $@ $<

$(OUT)/linux/smoke: tests/smoke.c tests/unixside.c $(OUT)/linux/libuwph.a
	$(CC_LINUX) $(CFLAGS_COMMON) -Isrc -pthread -o $@ tests/smoke.c tests/unixside.c $(OUT)/linux/libuwph.a

# tests/unixside.c stands in for Wine's Unix side (built with WINE_UNIX_LIB,
# calling the host exports the way ntdll.so would after libc_names.h)
$(OUT)/win/unixside.o: tests/unixside.c $(HDRS)
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_COMMON) $(WIN_DEFS) -DWINE_UNIX_LIB -c -o $@ $<

$(OUT)/win/smoke.exe: tests/smoke.c $(OUT)/win/unixside.o $(OUT)/win/libuwph.a
	$(CC_WIN) $(CFLAGS_COMMON) $(WIN_DEFS) -Isrc -o $@ tests/smoke.c $(OUT)/win/unixside.o \
	    $(OUT)/win/libuwph.a -lbcrypt -lsynchronization -lwindowsapp

# O2 traced set: each O1-priority symbol through its uwph entry + counter
$(OUT)/linux/traced: tests/traced.c $(OUT)/linux/libuwph.a
	$(CC_LINUX) $(CFLAGS_COMMON) -pthread -o $@ tests/traced.c $(OUT)/linux/libuwph.a

$(OUT)/win/traced.exe: tests/traced.c $(OUT)/win/libuwph.a
	$(CC_WIN) $(CFLAGS_COMMON) $(WIN_DEFS) -o $@ tests/traced.c \
	    $(OUT)/win/libuwph.a -lbcrypt -lsynchronization -lwindowsapp

# Bounded real Wine 10.0 inventory / binding diagnostic only. Unsupported imports
# remain visible and block full-coverage claims; this does not run a PE.
check: $(OUT)/linux/libuwph.a $(OUT)/win/libuwph.a
	$(MAKE) -C harness wine-surface-check

test: all
	$(OUT)/linux/abi_check
	$(OUT)/linux/smoke "$(CURDIR)/tests"
	WINEDEBUG=-all $(WINE) $(OUT)/win/smoke.exe "Z:$(subst /,\,$(CURDIR)/tests)"
	$(OUT)/linux/traced "$(CURDIR)/tests"
	WINEDEBUG=-all $(WINE) $(OUT)/win/traced.exe "Z:$(subst /,\,$(CURDIR)/tests)"
	$(MAKE) -C harness traced-test

clean:
	rm -rf $(OUT)

.PHONY: all check test clean
