CC = gcc
CFLAGS = -O3 -g3 -Wall -Wextra
LDFLAGS = -lgsl -lgslcblas -lm

VPATH=.:../mem

drama: drama.o mem.o
	$(CC) drama.o ../mem/mem.o -o drama $(LDFLAGS)

drama.o: drama.c
	gcc $(CFLAGS) -c drama.c

# TODO: this doesn't update mem.o if mem.c changes
mem.o:
	$(MAKE) -C ../mem

.PHONY: clean
clean:
	rm -f drama.o drama
