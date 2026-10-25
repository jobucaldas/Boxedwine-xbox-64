CC_WIN ?= x86_64-w64-mingw32-gcc
OUT := out
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

.PHONY: all smoke-exe ladder package offline-smoke clean
all: smoke-exe

# Loose layout fallback; no SDK packer or device activation is implied.
package: smoke-exe
	./pack.sh

smoke-exe: $(OUT)/smoke.exe

$(OUT)/smoke.exe: main.c Makefile
	mkdir -p $(OUT)
	$(CC_WIN) $(CFLAGS) -m64 -mconsole -static -o $@ $<

clean:
	rm -f $(OUT)/smoke.exe $(OUT)/ladder $(OUT)/run_via_wine

# Hosted Linux diagnostic; device activation remains separate.
ladder: $(OUT)/ladder

$(OUT)/ladder: ladder.py Makefile
	mkdir -p $(OUT)
	cp ladder.py $@
	chmod +x $@

# Linux-box smoke: package, offline ladder, payload exe via run_via_wine -> Wine.
offline-smoke: ladder
	./offline-smoke.sh
