CC = x86_64-w64-mingw32-gcc
PIN_VERSION := $(shell sed -n 's/^manifest_version=//p' PIN.txt)
PIN_SHA256 := $(shell sed -n 's/^steamclient64_sha256=//p' PIN.txt)
.PHONY: all clean
all: out/steamhost64.exe
out/steamhost64.exe: steamhost.c PIN.txt Makefile
	mkdir -p out
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DPIN_VERSION=$(PIN_VERSION)u -DPIN_SHA256='"$(PIN_SHA256)"' -o $@ $< -lbcrypt
clean:
	rm -f out/steamhost64.exe
