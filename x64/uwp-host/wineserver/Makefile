# Wine 10.0 server objects with private-copy hosted lifecycle patches.
#
#   make lib     configure $(WINE_BUILD) once, build $(OUT)/libwineserver.a
#   make clean   remove $(OUT) and the server objects in $(WINE_BUILD)
#
# Sources are Wine's own server/*.c, patched in private copies; main() is renamed to
# wine_server_main with objcopy so a host can link the archive.

OUT        ?= out
WINE_SRC   ?= /tmp/wine-src
WINE_BUILD ?= /workspace/wine-build
OBJCOPY    ?= objcopy
AR         ?= ar
OBJDIR     := $(OUT)/obj/hosted
COPYDIR    := $(OUT)/server-src
CONFIG_H   := $(WINE_BUILD)/include/config.h

# Source list from Wine's own server/Makefile.in SOURCES.
SRCS := $(filter %.c,$(shell sed -n '/^SOURCES/,/^$$/p' $(WINE_SRC)/server/Makefile.in))
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
HDRS := $(wildcard $(WINE_SRC)/server/*.h)

# Compiler and flags as configured (the makefile variables config.status
# carries), applied the way makedep does for server/ on x86_64
# (tools/makedep.c output_source_one_arch()).
# -fPIC is added so the archive can go into a PIE/shared host.
config_var = $(shell sed -n 's/^$(1) = *//p;T;q' $(WINE_BUILD)/config.status 2>/dev/null)
WINE_CC          = $(call config_var,CC)
WINE_CFLAGS      = $(call config_var,CFLAGS)
WINE_EXTRACFLAGS = $(call config_var,EXTRACFLAGS)
WINE_PREFIX      = $(call config_var,prefix)
SERVER_DEFS = -I$(OBJDIR) -I$(COPYDIR) -I$(WINE_BUILD)/include -I$(WINE_SRC)/include \
              -D__WINESRC__ -DWINE_UNIX_LIB -include $(CURDIR)/patches/hosted.h
# server/Makefile.in: unicode_EXTRADEFS
$(OBJDIR)/unicode.o: EXTRADEFS = -DBINDIR="\"$(WINE_PREFIX)/bin\"" -DDATADIR="\"$(WINE_PREFIX)/share\""

all: lib

lib: $(OUT)/libwineserver.a $(OUT)/protocol_version

$(CONFIG_H):
	WINE_SRC=$(WINE_SRC) WINE_BUILD=$(WINE_BUILD) ./build.sh configure

$(COPYDIR)/.prepared: build.sh $(wildcard patches/*) $(wildcard $(WINE_SRC)/server/*.[ch] $(WINE_SRC)/server/protocol.def) $(WINE_SRC)/tools/make_requests
	WINE_SRC=$(WINE_SRC) WINE_BUILD=$(WINE_BUILD) ./build.sh prepare $(abspath $(COPYDIR))
	@touch $@

$(OBJDIR)/%.o: $(WINE_SRC)/server/%.c $(HDRS) $(CONFIG_H) $(COPYDIR)/.prepared
	@test -n "$(WINE_CC)" || { echo "no CC in $(WINE_BUILD)/config.status" >&2; exit 1; }
	@mkdir -p $(@D)
	$(WINE_CC) -c -o $@ $(COPYDIR)/$*.c $(SERVER_DEFS) $(EXTRADEFS) $(WINE_EXTRACFLAGS) -fPIC $(WINE_CFLAGS)

$(OUT)/obj/wine_server_main.o: $(OBJDIR)/main.o
	@mkdir -p $(@D)
	$(OBJCOPY) --redefine-sym main=wine_server_main $< $@

$(OUT)/libwineserver.a: $(filter-out $(OBJDIR)/main.o,$(OBJS)) $(OUT)/obj/wine_server_main.o
	rm -f $@
	$(AR) rcs $@ $^

# build.sh prepare regenerates the protocol header as Debian does (version 1).
$(OUT)/protocol_version: $(COPYDIR)/.prepared
	@mkdir -p $(@D)
	grep -h '#define SERVER_PROTOCOL_VERSION' $(COPYDIR)/wine/server_protocol.h > $@

clean:
	rm -rf $(OUT) $(OBJS)

.PHONY: all lib clean

$(OUT)/test_host: test_host.c server_thread.c $(OUT)/libwineserver.a
	$(CC) -Wall -Wextra -g -o $@ test_host.c server_thread.c $(OUT)/libwineserver.a -pthread

test-host: $(OUT)/test_host
.PHONY: test-host
