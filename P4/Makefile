CC = gcc
CFLAGS = -g -Wall -fsanitize=address,undefined

all: nimd tests

tests: tests.c
	$(CC) $(CFLAGS) -o tests tests.c

nimd: nimd.c
	$(CC) $(CFLAGS) -o nimd nimd.c	