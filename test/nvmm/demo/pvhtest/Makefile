all: /tmp/pvhtest.elf

/tmp/pvhtest.elf: guest.S guest.ld
	cc -c -o /tmp/pvhtest.o guest.S
	ld -T guest.ld -o ${.TARGET} /tmp/pvhtest.o

clean:
	rm -f /tmp/pvhtest.o /tmp/pvhtest.elf
