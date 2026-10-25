# B44 O2 TLS provider spike (decision (a) in ../docs/tls-on-xbox.md).
#
#   make -C x64/uwp-host/tls test    cross-build GnuTLS for the Win32 backend,
#                                    link it into a uwph registry entry, run
#                                    provider_test.exe (off-device: wine64)
#   make -C x64/uwp-host/tls spike   Linux stand-in: UWPH_DENY_DL=gnutls
#                                    net_probe via launcher -> Wine -> uwph shim
#
# Not part of x64/uwp-host `make test`: the first build fetches and compiles
# pinned GnuTLS/nettle/GMP releases (a few minutes).

OUT     := out
CC_WIN  ?= x86_64-w64-mingw32-gcc
WINE    ?= $(shell command -v wine64 2>/dev/null || { test -x /usr/lib/wine/wine64 && printf /usr/lib/wine/wine64; } || command -v wine)
WIN_DEFS := -DWINAPI_FAMILY=2 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008
CFLAGS  := -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -I../include -I$(OUT)/win/include
WIN_LIBS := $(OUT)/win/lib/libgnutls.a $(OUT)/win/lib/libhogweed.a $(OUT)/win/lib/libnettle.a $(OUT)/win/lib/libgmp.a \
            -lws2_32 -lcrypt32 -lbcrypt -lncrypt -ladvapi32 -lsynchronization -lwindowsapp

all: $(OUT)/provider_test.exe

$(OUT)/linux/lib/libgnutls.so.30:
	./build-gnutls.sh linux

$(OUT)/win/lib/libgnutls.a:
	./build-gnutls.sh win

../out/win/libuwph.a:
	$(MAKE) -C .. out/win/libuwph.a

$(OUT)/provider_test.exe: provider_test.c gnutls_provider.c wine_gnutls_symbols.h $(OUT)/win/lib/libgnutls.a ../out/win/libuwph.a
	$(CC_WIN) $(CFLAGS) $(WIN_DEFS) -static -o $@ provider_test.c gnutls_provider.c ../out/win/libuwph.a $(WIN_LIBS)

check-symbols:
	./check-symbols.sh

test: check-symbols $(OUT)/provider_test.exe
	x86_64-w64-mingw32-objdump -p $(OUT)/provider_test.exe | grep 'DLL Name' | sort
	WINEDEBUG=-all $(WINE) $(OUT)/provider_test.exe

spike: $(OUT)/linux/lib/libgnutls.so.30
	../harness/tls-evidence/spike.sh

.PHONY: all check-symbols test spike
