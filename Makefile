CC      = cc
TARGET  = get_file_list
SRC     = get_file_list.c
CFLAGS  = -Wall -Wextra -O2

DEBUG_FLAGS = -g -O0 -DDEBUG -fsanitize=address,undefined,leak -fno-omit-frame-pointer

.PHONY: all debug clean

all: $(TARGET)

debug: CFLAGS = -Wall -Wextra $(DEBUG_FLAGS)
debug: LDFLAGS += -fsanitize=address,undefined,leak
debug: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
