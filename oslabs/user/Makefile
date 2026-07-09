# Build NJU-OS user programs for the AbstractMachine *native* backend.
#
# Each program is a freestanding, statically-linked flat binary whose code +
# rodata + data live at 0x40000000 (the base of the AM native user address
# space).  The kernel loader maps zeroed pages there and copies the flat image,
# so .bss is naturally zero.  genprogs.py embeds every binary into
# ../kernel/src/uprogs.inc, which uproc.c #includes.

CROSS_COMPILE = x86_64-linux-gnu-
CC  = $(CROSS_COMPILE)gcc
LD  = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

CFLAGS  = -Os -m64 -fno-pic -mno-sse -mno-red-zone -fcf-protection=none
CFLAGS += -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -Wall
CFLAGS += -fno-asynchronous-unwind-tables -fno-jump-tables

LDFLAGS = -static -T user.ld --no-dynamic-linker

# Add new programs here.
PROGS = init hello forktest

all: $(PROGS:%=%.bin)
	python3 genprogs.py $(PROGS)

start.o: start.S ../kernel/framework/syscall.h
	$(CC) $(CFLAGS) -c start.S -o start.o

%.o: %.c ulib.h ../kernel/framework/syscall.h ../kernel/framework/user.h
	$(CC) $(CFLAGS) -c $*.c -o $*.o

# start.o must come first so _start is at offset 0 of the flat image.
%.bin: %.o start.o user.ld
	$(LD) $(LDFLAGS) -o $*.elf start.o $*.o
	$(OBJCOPY) -O binary $*.elf $*.bin

clean:
	rm -f *.o *.elf *.bin

.PHONY: all clean
