CC = x86_64-w64-mingw32-gcc
# PIN_FILE/OUT retain the isolated legacy runners; normal builds use the mapped pin.
PIN ?= $(if $(PIN_FILE),$(PIN_FILE),PIN-1788652215.txt)
OUT ?= out
PIN_VERSION := $(shell sed -n 's/^manifest_version=//p' $(PIN))
PIN_SHA256 := $(shell sed -n 's/^steamclient64_sha256=//p' $(PIN))
MAP ?= ../../../$(shell sed -n 's/^map_file=//p' $(PIN))
CLIENT ?= out/client/steamclient64.dll
MAP_REL := $(shell sed -n 's/^map_file=//p' $(PIN))
ifneq ($(MAP_REL),)
HOST_DEFS = -I$(OUT) -DHOST_MAP_HEADER='"host-map.h"'
HOST_DEPS = $(OUT)/host-map.h
else
OSW_TABLES_BUILD := $(shell sed -n 's/^osw_tables_build=//p' $(PIN))
HOST_DEFS = -DHOST_LEGACY_TABLES -DOSW_TABLES_BUILD=$(OSW_TABLES_BUILD)u
endif
.PHONY: all clean test-denylist FORCE
all: $(OUT)/steamhost64.exe
$(OUT)/host-map.h: FORCE $(MAP) $(CLIENT) ../ifacemap/generate-host.py ../ifacemap/validate-map.py ../ifacemap/denylist.py
	python3 ../ifacemap/validate-map.py --map $(MAP) --dll $(CLIENT)
	python3 ../ifacemap/generate-host.py $(MAP) $(CLIENT) $@
$(OUT)/steamhost64.exe: steamhost.c slots.h $(HOST_DEPS) $(PIN) Makefile
	mkdir -p $(OUT)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(HOST_DEFS) -DPIN_VERSION=$(PIN_VERSION)u -DPIN_SHA256='"$(PIN_SHA256)"' -o $@ $< -lbcrypt
test-denylist:
	python3 test-denylist.py
clean:
	rm -f $(OUT)/steamhost64.exe $(OUT)/host-map.h

FORCE:
