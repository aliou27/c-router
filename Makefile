CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -std=c17 -Iinclude

TARGET = c-router

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

.PHONY: all clean run lab-up lab-down

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

lab-up:
	sudo ./scripts/netns-up.sh

lab-down:
	sudo ./scripts/netns-down.sh