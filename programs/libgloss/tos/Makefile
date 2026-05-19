CC = i386-elf-gcc
AR = i386-elf-ar
CFLAGS = -m32 -ffreestanding -nostdlib -O2 -Wall -I.

OBJS = syscalls.o crt0.o

all: libtos.a crt0.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

libtos.a: $(OBJS)
	$(AR) rcs $@ $^

clean:
	rm -f *.o *.a
