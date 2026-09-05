CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags MagickWand)
LDFLAGS = $(shell pkg-config --libs MagickWand) -lmagic -lcrypto -lpthread
DEBUG_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O0 -DDEBUG
CFLAGS  += -I/usr/local/src/clip.cpp -I/usr/local/src/clip.cpp/ggml/include
LDFLAGS += -L/usr/local/src/clip.cpp/build -L/usr/local/src/clip.cpp/build/ggml/src -lclip -lggml -lstdc++ -lm -lpq -lpthread

TARGET = wallclip
SRC = wallclip.c

.PHONY: all debug clean

all: clean compile

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
