CC = gcc
CFLAGS = -Wall -pthread

all: assignment3

assignment3: assignment3.c
	$(CC) $(CFLAGS) -o assignment3 assignment3.c

clean:
	rm -f assignment3 *.o book_*.txt