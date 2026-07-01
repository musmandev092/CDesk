# DankC — fallback build (gcc + wayland-scanner). The primary build system is
# Meson (meson.build); this Makefile exists for quick bring-up before meson is
# installed. `make` produces ./bin/dankc.

CC          ?= cc
PKG_CONFIG  ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PKGS := wayland-client wayland-egl egl glesv2

WARNINGS := -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wno-unused-parameter
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS   += $(WARNINGS) -Isrc -Iprotocol/generated $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

PROTO_XML := $(wildcard protocol/*.xml)
PROTO_H   := $(patsubst protocol/%.xml,protocol/generated/%-client-protocol.h,$(PROTO_XML))
PROTO_C   := $(patsubst protocol/%.xml,protocol/generated/%-protocol.c,$(PROTO_XML))
PROTO_O   := $(PROTO_C:.c=.o)

SRC := $(wildcard src/*.c src/core/*.c src/wayland/*.c src/ui/bar/*.c)
OBJ := $(SRC:.c=.o)

BIN := bin/dankc

all: $(BIN)

$(BIN): $(OBJ) $(PROTO_O)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDLIBS)

# Generated Wayland protocol glue.
protocol/generated/%-client-protocol.h: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) client-header $< $@

protocol/generated/%-protocol.c: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) private-code $< $@

# All translation units need the generated client headers present first.
$(OBJ): $(PROTO_H)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin protocol/generated $(OBJ) $(PROTO_O)

.PHONY: all clean
