CFLAGS := -g -O -Wall -pedantic -std=c11
CC := gcc

source := signals.c
name := signals

trash := ${source:.c=.o}

all:
	$(CC) $(CFLAGS) -o $(name) $(source)

clean:
	@- $(RM) $(name) $(trash)
