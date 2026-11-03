CC ?= cc
PREFIX ?= /usr
CLIP_DIR ?= /usr/local/src/clip.cpp
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags MagickWand)
LDFLAGS = $(shell pkg-config --libs MagickWand) -lmagic -lcrypto -lpthread
DEBUG_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O0 -DDEBUG
CFLAGS  += -I$(CLIP_DIR) -I$(CLIP_DIR)/ggml/include
LDFLAGS += -L$(CLIP_DIR)/build -L$(CLIP_DIR)/build/ggml/src -lclip -lggml -lm -lpq -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDFLAGS += -lc++        # macOS ships no libstdc++
else
LDFLAGS += -lstdc++
endif

TARGET = wallclip
SRC = wallclip.c env-loader.c

.PHONY: all debug install clean test test-asan

all: clean compile install

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

install:
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

TEST_SRC = tests/test_platform.c
TEST_DEPS = $(TEST_SRC) env-loader.c env-loader.h log.h platform.h
TEST_CFLAGS = -Wall -Wextra -g

# Platform unit tests. They need no clip.cpp, ImageMagick or PostgreSQL.
# On FreeBSD run them with gmake, this makefile uses GNU make syntax.
test: $(TEST_DEPS)
	$(CC) $(TEST_CFLAGS) -O1 -o tests/test_platform $(TEST_SRC)
	./tests/test_platform

test-asan: $(TEST_DEPS)
	$(CC) $(TEST_CFLAGS) -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -o tests/test_platform_asan $(TEST_SRC)
	./tests/test_platform_asan

clean:
	rm -f $(TARGET) tests/test_platform tests/test_platform_asan
