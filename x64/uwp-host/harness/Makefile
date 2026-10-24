CC ?= gcc
OUT := out
CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC -pthread -DUWPH_BACKEND_LINUX_PASSTHROUGH -I../include
SOURCES := $(wildcard ../src/*.c) linux_backend.c
HEADERS := $(wildcard ../include/uwph/*.h ../src/*.h)
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
vpath %.c ../src .
$(OUT)/%.o: %.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/libuwph.a: $(OBJECTS)
	ar rcs $@ $^
$(OUT)/backend_test: backend_test.c $(OUT)/libuwph.a
	$(CC) $(CFLAGS) $< $(OUT)/libuwph.a -o $@
backend-test: $(OUT)/backend_test
	timeout 15s $(OUT)/backend_test
.PHONY: backend-test
