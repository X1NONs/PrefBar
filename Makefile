ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error Define PS5_PAYLOAD_SDK)
endif

all: perfbar.elf

perfbar.elf: perfbar.c perf_wire.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ perfbar.c

clean:
	rm -f perfbar.elf
