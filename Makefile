CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lcurl -lmrss

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

zblock: $(OBJ) /usr/local/lib/libdiscord.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

.PHONY: clean
clean: rm -f $(OBJ) linuxbot
