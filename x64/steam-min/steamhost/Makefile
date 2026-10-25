CC = x86_64-w64-mingw32-gcc
PIN ?= PIN-1788652215.txt
PIN_VERSION := $(shell sed -n 's/^manifest_version=//p' $(PIN))
PIN_SHA256 := $(shell sed -n 's/^steamclient64_sha256=//p' $(PIN))
MAP ?= ../../../$(shell sed -n 's/^map_file=//p' $(PIN))
CLIENT ?= out/client/steamclient64.dll
.PHONY: all clean test-denylist FORCE
all: out/steamhost64.exe
out/host-map.h: FORCE $(MAP) $(CLIENT) ../ifacemap/generate-host.py ../ifacemap/validate-map.py ../ifacemap/denylist.py
	python3 ../ifacemap/validate-map.py --map $(MAP) --dll $(CLIENT)
	python3 ../ifacemap/generate-host.py $(MAP) $(CLIENT) $@
out/steamhost64.exe: steamhost.c slots.h out/host-map.h $(PIN) Makefile
	mkdir -p out
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DPIN_VERSION=$(PIN_VERSION)u -DPIN_SHA256='"$(PIN_SHA256)"' -o $@ $< -lbcrypt
test-denylist:
	python3 test-denylist.py
clean:
	rm -f out/steamhost64.exe out/host-map.h

FORCE:
