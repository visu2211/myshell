CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

all: mysh

mysh: mysh.c
	$(CC) $(CFLAGS) -o $@ $<

test: mysh
	python3 tests/test_mysh.py

clean:
	rm -f mysh

.PHONY: all test clean
