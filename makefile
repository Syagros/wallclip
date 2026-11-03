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

.PHONY: all debug install clean

all: clean compile install

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

install:
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
