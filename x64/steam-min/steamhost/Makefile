CC = x86_64-w64-mingw32-gcc
PIN_FILE ?= PIN.txt
OUT ?= out
PIN_VERSION := $(shell sed -n 's/^manifest_version=//p' $(PIN_FILE))
PIN_SHA256 := $(shell sed -n 's/^steamclient64_sha256=//p' $(PIN_FILE))
OSW_TABLES_BUILD := $(shell sed -n 's/^osw_tables_build=//p' $(PIN_FILE))
.PHONY: all clean
all: $(OUT)/steamhost64.exe
$(OUT)/steamhost64.exe: steamhost.c $(PIN_FILE) Makefile
	mkdir -p $(OUT)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DPIN_VERSION=$(PIN_VERSION)u -DPIN_SHA256='"$(PIN_SHA256)"' -DOSW_TABLES_BUILD=$(OSW_TABLES_BUILD)u -o $@ $< -lbcrypt
clean:
	rm -f $(OUT)/steamhost64.exe
