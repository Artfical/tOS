CC = /system/pkg/gcc/bin/gcc
all: prog
prog: a.o b.o
	$(CC) -o prog a.o b.o
%.o: %.c
	$(CC) -c $<
