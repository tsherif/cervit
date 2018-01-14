CC=gcc 
CFLAGS=-g -Wall -Wextra -Wno-unused-parameter -pedantic 
LDLIBS=-pthread

cervit: cervit.c
	$(CC) $(CFLAGS) -o cervit cervit.c $(LDLIBS)
