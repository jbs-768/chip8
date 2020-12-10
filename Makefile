TARGET = chip
SRC = chip.c
OBJ = $(SRC:.c=.o)

CC = gcc
CFLAGS = -std=gnu11 -march=native -pedantic -Wall -Wextra -Wdouble-promotion\
				 -Wfloat-conversion -Wno-error=unused-function\
				 -Wno-error=unused-parameter -Wno-error=unused-variable
LFLAGS = -lSDL2


$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LFLAGS)

.PHONY: clean
clean:
	rm $(OBJ) $(TARGET)

