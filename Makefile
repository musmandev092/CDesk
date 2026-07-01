# DankC — fallback build (gcc + wayland-scanner). The primary build system is
# Meson (meson.build); this Makefile exists for quick bring-up before meson is
# installed. `make` produces ./bin/dankc.

CC          ?= cc
PKG_CONFIG  ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PKGS := wayland-client wayland-egl egl glesv2

WARNINGS := -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wno-unused-parameter
INCLUDES := -Isrc -Iprotocol/generated -Ithird_party/nanovg -Ithird_party/nanosvg -Ithird_party/cjson
BASE_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE $(INCLUDES) \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))

CFLAGS   ?= -O2 -g
CFLAGS   += $(BASE_CFLAGS) $(WARNINGS)

# Vendored third-party code (nanovg, cJSON): compile with warnings suppressed so
# our own -Wextra output stays meaningful.
TP_CFLAGS := -O2 -g $(BASE_CFLAGS) -w

LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

PROTO_XML := $(wildcard protocol/*.xml)
PROTO_H   := $(patsubst protocol/%.xml,protocol/generated/%-client-protocol.h,$(PROTO_XML))
PROTO_C   := $(patsubst protocol/%.xml,protocol/generated/%-protocol.c,$(PROTO_XML))
PROTO_O   := $(PROTO_C:.c=.o)

SRC := $(wildcard src/*.c src/core/*.c src/wayland/*.c src/render/*.c src/ui/*.c src/ui/bar/*.c \
	src/services/*.c src/niri/*.c src/theme/*.c src/ipc/*.c)
OBJ := $(SRC:.c=.o)

TP_SRC := third_party/nanovg/nanovg.c third_party/nanovg/nanovg_gl_impl.c \
	third_party/nanosvg/nanosvg_impl.c third_party/cjson/cJSON.c
TP_OBJ := $(TP_SRC:.c=.o)

BIN := bin/dankc

all: $(BIN)

$(BIN): $(OBJ) $(TP_OBJ) $(PROTO_O)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDLIBS)

# Generated Wayland protocol glue.
protocol/generated/%-client-protocol.h: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) client-header $< $@

protocol/generated/%-protocol.c: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) private-code $< $@

# All first-party translation units need the generated client headers first.
$(OBJ): $(PROTO_H)

# Vendored code: relaxed warnings.
$(TP_OBJ): %.o: %.c
	$(CC) $(TP_CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin protocol/generated $(OBJ) $(TP_OBJ) $(PROTO_O)

.PHONY: all clean
