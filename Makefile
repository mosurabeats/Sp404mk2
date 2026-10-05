CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -Iinclude
LDLIBS += -lm

LIB_SRC := src/doomfx.c src/groove.c $(wildcard src/fx/*.c)
LIB_OBJ := $(LIB_SRC:%.c=build/%.o)

all: build/doomfx build/test_doomfx

build/%.o: %.c src/dsp.h include/doomfx.h include/doomfx_groove.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/libdoomfx.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

build/doomfx: build/cli/main.o build/cli/wav.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/test_doomfx: build/tests/test_doomfx.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: build/test_doomfx
	./build/test_doomfx

clean:
	rm -rf build

.PHONY: all test clean
