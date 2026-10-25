CC := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

all: out/xa2_probe.exe
out/xa2_probe.exe: xa2_probe.c
	mkdir -p out
	$(CC) $(CFLAGS) $< -o $@ -lole32 -lm
clean:
	rm -rf out
.PHONY: all clean
