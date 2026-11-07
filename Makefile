CC ?= cc
CFLAGS ?= -O3 -march=native -Wall -Wextra -std=c11

rd: rd.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f rd

.PHONY: clean
