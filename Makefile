CC = cc
CFLAGS =

all: lszorro test_execbase

lszorro: lszorro.o
	$(CC) -o lszorro lszorro.o

lszorro.o: lszorro.c zorro_ids.h
	$(CC) $(CFLAGS) -c lszorro.c

test_execbase: test_execbase.o
	$(CC) -o test_execbase test_execbase.o

test_execbase.o: test_execbase.c
	$(CC) $(CFLAGS) -c test_execbase.c

clean:
	rm -f lszorro test_execbase lszorro.o test_execbase.o
