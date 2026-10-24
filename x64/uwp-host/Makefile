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
HDRS     := $(wildcard include/uwph/*.h) src/internal.h
CFLAGS_COMMON := -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude

CC_LINUX ?= gcc
CC_WIN   ?= x86_64-w64-mingw32-gcc
WINE     ?= wine64
# WINAPI_FAMILY_APP = 2; Windows 10 2004 headers level
WIN_DEFS := -DWINAPI_FAMILY=2 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008

LIN_OBJS := $(SRCS:src/%.c=$(OUT)/linux/%.o)
WIN_OBJS := $(SRCS:src/%.c=$(OUT)/win/%.o)

all: $(OUT)/linux/libuwph.a $(OUT)/linux/smoke $(OUT)/linux/abi_check \
     $(OUT)/win/libuwph.a $(OUT)/win/smoke.exe check

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

# every libc import of the installed wine64 Unix side must be classified in
# tools/surface.tsv, and every "host" entry must exist as uwph_<name> in both builds
check: $(OUT)/linux/libuwph.a $(OUT)/win/libuwph.a tools/surface.tsv
	tools/check_surface.sh $(OUT)/linux/libuwph.a $(OUT)/win/libuwph.a

test: all
	$(OUT)/linux/abi_check
	$(OUT)/linux/smoke
	WINEDEBUG=-all $(WINE) $(OUT)/win/smoke.exe

clean:
	rm -rf $(OUT)

.PHONY: all check test clean
