CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags MagickWand)
LDFLAGS = $(shell pkg-config --libs MagickWand) -lmagic
DEBUG_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O0

TARGET = wallclip
SRC = main.c

.PHONY: all debug clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $(TARGET)_debug $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET)_debug
