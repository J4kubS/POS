CFLAGS := -g -O -Wall -pedantic -std=c11
LFLAGS := -lpthread
CC := gcc

source := shell.c
name := shell

trash := ${source:.c=.o}

all:
	$(CC) $(CFLAGS) -o $(name) $(source) $(LFLAGS)

clean:
	@- $(RM) $(name) $(trash)
