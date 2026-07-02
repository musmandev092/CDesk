# DankC — fallback build (gcc + wayland-scanner). The primary build system is
# Meson (meson.build); this Makefile exists for quick bring-up before meson is
# installed. `make` produces ./bin/dankc.

CC          ?= cc
CXX         ?= c++
PKG_CONFIG  ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PKGS := wayland-client wayland-egl egl glesv2 libsystemd xkbcommon pam fontconfig

WARNINGS := -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wno-unused-parameter
INCLUDES := -Isrc -Iprotocol/generated -Ithird_party/nanovg -Ithird_party/nanosvg -Ithird_party/cjson
BASE_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE $(INCLUDES) \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))

CFLAGS   ?= -O2 -g
# -MMD -MP: emit .d dependency files so header edits rebuild every affected
# object. Without this, a struct change in a header leaves stale .o files with
# the OLD field offsets silently linked together (caused real cross-module
# memory "corruption" after a merge touched core/config.h).
CFLAGS   += $(BASE_CFLAGS) $(WARNINGS) -MMD -MP

# C++ is used for exactly one module (theme/dynamic.cpp, Material colour math).
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 $(INCLUDES) $(shell $(PKG_CONFIG) --cflags $(PKGS)) -Wall -Wextra \
	-Wno-unused-parameter -MMD -MP

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
	$(CXX) -o $@ $^ $(LDLIBS)

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

.PHONY: all clean test-calc
