CC = gcc
CFLAGS = -O3 -g3 -Wall -Wextra
LDFLAGS = -lgsl -lgslcblas -lm

VPATH=.:../mem:../drama

pattern: pattern.o drama.o mem.o
	$(CC) pattern.o ../drama/drama.o ../mem/mem.o -o pattern $(LDFLAGS)

pattern.o: pattern.c drama.h

mem.o:
	$(MAKE) -C ../mem

# TODO: this doesn't update drama.o if drama.c changes
drama.o:
	$(MAKE) -C ../drama


.PHONY: clean
clean:
	rm -f pattern.o pattern
