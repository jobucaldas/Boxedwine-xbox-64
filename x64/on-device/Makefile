CC_WIN ?= x86_64-w64-mingw32-gcc
OUT := out
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

.PHONY: all smoke-exe clean
all: smoke-exe

smoke-exe: $(OUT)/smoke.exe

$(OUT)/smoke.exe: main.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -m64 -mconsole -static -o $@ $<

clean:
	rm -f $(OUT)/smoke.exe
