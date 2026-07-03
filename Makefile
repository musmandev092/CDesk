# DankC — fallback build (gcc + wayland-scanner). The primary build system is
# Meson (meson.build); this Makefile exists for quick bring-up before meson is
# installed. `make` produces ./bin/dankc.

CC          ?= cc
CXX         ?= c++
PKG_CONFIG  ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PKGS := wayland-client wayland-egl egl glesv2 libsystemd xkbcommon pam fontconfig harfbuzz

# FriBidi (real UAX#9 BiDi reordering, see src/render/shape.c) is optional at
# build time: if pkg-config can't find it, shape.c falls back to its
# documented "UBA-lite" approximation (contiguous RTL segments reversed,
# numbers kept LTR) instead of failing the build.
HAVE_FRIBIDI := $(shell $(PKG_CONFIG) --exists fribidi && echo 1)
ifeq ($(HAVE_FRIBIDI),1)
PKGS += fribidi
FRIBIDI_CFLAGS := -DDC_HAVE_FRIBIDI=1
endif

WARNINGS := -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wno-unused-parameter
INCLUDES := -Isrc -Iprotocol/generated -Ithird_party/nanovg -Ithird_party/nanosvg -Ithird_party/cjson
BASE_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE $(INCLUDES) $(FRIBIDI_CFLAGS) \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))

# RELEASE=1 selects the shipped/packaged optimization profile: no -g (smaller,
# no debug bloat), -DNDEBUG (strips assert() overhead in vendored nanovg/
# nanosvg/cJSON — first-party code has no asserts but third_party does),
# -fno-plt (skip the PLT indirection on every D-Bus/GL call site) and
# -fvisibility=hidden (smaller symbol table, lets the compiler devirtualize/
# inline more aggressively across TUs since it can prove nothing outside the
# binary references a given symbol — safe here, dankc is an executable, not a
# shared library, so nothing needs those symbols exported).
#
# -flto=auto is on in BOTH profiles (dev and release): it lets gcc inline
# across translation units at link time, which matters on the hot
# text-shaping (render/shape.c) and nanovg-tessellation call paths that cross
# .c file boundaries. -flto must be passed at both compile *and* link time,
# or the compiler silently falls back to non-LTO codegen.
#
# Dev and release builds are NOT flag-compatible at the object-file level
# (LTO bytecode + -DNDEBUG differ) — always `make clean` before switching
# profiles (the `release` target below does this for you).
RELEASE ?= 0
ifeq ($(RELEASE),1)
OPT_FLAGS := -O2 -flto=auto -DNDEBUG -fno-plt -fvisibility=hidden
else
OPT_FLAGS := -O2 -g -flto=auto
endif

CFLAGS   ?= $(OPT_FLAGS)
# -MMD -MP: emit .d dependency files so header edits rebuild every affected
# object. Without this, a struct change in a header leaves stale .o files with
# the OLD field offsets silently linked together (caused real cross-module
# memory "corruption" after a merge touched core/config.h).
CFLAGS   += $(BASE_CFLAGS) $(WARNINGS) -MMD -MP

# C++ is used for exactly one module (theme/dynamic.cpp, Material colour math).
CXXFLAGS ?= $(OPT_FLAGS)
CXXFLAGS += -std=c++17 $(INCLUDES) $(shell $(PKG_CONFIG) --cflags $(PKGS)) -Wall -Wextra \
	-Wno-unused-parameter -MMD -MP

# Vendored third-party code (nanovg, cJSON): compile with warnings suppressed so
# our own -Wextra output stays meaningful. Same optimization profile so its
# objects stay LTO-compatible with the rest of the binary.
TP_CFLAGS := $(OPT_FLAGS) $(BASE_CFLAGS) -w

LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
# LTO needs -flto at link time too (it's where the actual cross-TU
# optimization/codegen happens); -O2/-DNDEBUG/-fno-plt/-fvisibility on the
# link line are harmless no-ops for the linker but keep the link driver
# invocation consistent with how the objects were compiled.
LDFLAGS  ?= $(OPT_FLAGS)

PROTO_XML := $(wildcard protocol/*.xml)
PROTO_H   := $(patsubst protocol/%.xml,protocol/generated/%-client-protocol.h,$(PROTO_XML))
PROTO_C   := $(patsubst protocol/%.xml,protocol/generated/%-protocol.c,$(PROTO_XML))
PROTO_O   := $(PROTO_C:.c=.o)

SRC := $(wildcard src/*.c src/core/*.c src/wayland/*.c src/render/*.c src/ui/*.c src/ui/bar/*.c \
	src/services/*.c src/niri/*.c src/theme/*.c src/ipc/*.c)
OBJ := $(SRC:.c=.o)

CXX_SRC := $(wildcard src/theme/*.cpp)
CXX_OBJ := $(CXX_SRC:.cpp=.o)

TP_SRC := third_party/nanovg/nanovg.c third_party/nanovg/nanovg_gl_impl.c \
	third_party/nanosvg/nanosvg_impl.c third_party/cjson/cJSON.c
TP_OBJ := $(TP_SRC:.c=.o)

BIN := bin/dankc

all: $(BIN)

# Link with the C++ driver so libstdc++ is pulled in for the one C++ module.
$(BIN): $(OBJ) $(CXX_OBJ) $(TP_OBJ) $(PROTO_O)
	@mkdir -p bin
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Generated Wayland protocol glue.
protocol/generated/%-client-protocol.h: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) client-header $< $@

protocol/generated/%-protocol.c: protocol/%.xml
	@mkdir -p protocol/generated
	$(WAYLAND_SCANNER) private-code $< $@

# All first-party translation units need the generated client headers first.
$(OBJ): $(PROTO_H)
$(CXX_OBJ): $(PROTO_H)

src/theme/%.o: src/theme/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Vendored code: relaxed warnings.
$(TP_OBJ): %.o: %.c
	$(CC) $(TP_CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin protocol/generated $(OBJ) $(CXX_OBJ) $(TP_OBJ) $(PROTO_O) $(DEPS) bin/test_calc

DEPS := $(OBJ:.o=.d) $(CXX_OBJ:.o=.d)
-include $(DEPS)

# Standalone unit test for the launcher's math evaluator (src/services/calc.c)
# -- deliberately built with none of the Wayland/EGL/protocol machinery, just
# the evaluator + its test driver, so it can run in any environment (no
# niri/compositor needed).
bin/test_calc: tests/test_calc.c src/services/calc.c src/services/calc.h
	@mkdir -p bin
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc $(WARNINGS) -O2 -g \
		-o $@ tests/test_calc.c src/services/calc.c -lm

test-calc: bin/test_calc
	./bin/test_calc

# Shipped/packaged build: -O2 -flto -DNDEBUG, no -g (see OPT_FLAGS above).
# Object files from a dev build aren't LTO/flag-compatible, so always start
# from clean.
release:
	$(MAKE) clean
	$(MAKE) RELEASE=1 all

.PHONY: all clean test-calc release
