CC = gcc
CFLAGS = -g -Wall -std=c99 -fsanitize=address,undefined

nimd: nimd.c
	$(CC) $(CFLAGS) -o nimd nimd.c
	
rawc: rawc.o pbuf.o network.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f nimd rawwc *.o
