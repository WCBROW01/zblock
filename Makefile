CFLAGS = -Wall -Wextra -std=gnu11 -O2
LDFLAGS = -lpthread -lcurl -lmrss -lpq

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

zblock: $(OBJ) /usr/local/lib/libdiscord.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

.PHONY: clean
clean: rm -f $(OBJ) linuxbot
