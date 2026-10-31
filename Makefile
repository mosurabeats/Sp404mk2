CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -Iinclude
LDLIBS += -lm

LIB_SRC := src/doomfx.c src/groove.c $(wildcard src/fx/*.c) $(wildcard src/synth/*.c) $(wildcard src/os/*.c)
LIB_OBJ := $(LIB_SRC:%.c=build/%.o)

all: build/doomfx build/mosura build/test_doomfx build/test_mos

HEADERS := $(wildcard include/*.h) $(wildcard src/*.h) $(wildcard src/os/*.h) $(wildcard cli/*.h)

build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/libdoomfx.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

build/doomfx: build/cli/main.o build/cli/wav.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/mosura: build/cli/mosura.o build/cli/kit.o build/cli/png.o build/cli/wav.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/test_doomfx: build/tests/test_doomfx.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/test_mos: build/tests/test_mos.o build/libdoomfx.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: build/test_doomfx build/test_mos
	./build/test_doomfx
	./build/test_mos

clean:
	rm -rf build

.PHONY: all test clean
