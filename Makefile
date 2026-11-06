CC ?= cc
CXX ?= c++
AR ?= ar
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -Isrc
WARNINGS = -Wall -Wextra -Wpedantic -Werror

LIB_SRC = src/screen.c src/input.c src/utf8.c
LIB_OBJ = $(LIB_SRC:src/%.c=build/%.o)
HEADERS = include/minicell.h include/minicell_tcell.h $(wildcard src/*.h)
TESTS = build/utf8_test build/input_test build/screen_test build/tetris_test build/cpp_test

.PHONY: all test sanitize regenerate clean

all: build/libminicell.a build/demo build/tetris

build/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c $< -o $@

build/libminicell.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

build/%: examples/%.c build/libminicell.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libminicell.a -o $@

build/%_test: tests/%_test.c tests/check.h build/libminicell.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libminicell.a -o $@

# White-box test: includes src/screen.c directly.
build/screen_test: tests/screen_test.c tests/check.h src/screen.c build/libminicell.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
		$(filter-out build/screen.o,$(LIB_OBJ)) -o $@

# Includes examples/tetris.c directly.
build/tetris_test: tests/tetris_test.c tests/check.h examples/tetris.c build/libminicell.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libminicell.a -o $@

build/cpp_test: tests/cpp_test.cpp build/libminicell.a
	$(CXX) -std=c++11 $(CPPFLAGS) $(CXXFLAGS) $(WARNINGS) $< build/libminicell.a -o $@

test: $(TESTS) build/demo build/tetris
	@for t in $(TESTS); do echo "./$$t"; ./$$t || exit 1; done
	python3 tests/pty_test.py

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
		CXXFLAGS='-O1 -g -fsanitize=address,undefined' $(TESTS)
	@for t in $(TESTS); do echo "./$$t"; ./$$t || exit 1; done
	$(MAKE) clean

regenerate:
	python3 tools/generate.py

clean:
	rm -rf build
