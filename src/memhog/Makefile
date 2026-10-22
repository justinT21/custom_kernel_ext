# Buildroot passes CC, CFLAGS and LDFLAGS on the make command line, which
# overrides plain assignments here, hence "override". They include -fPIC and
# -Wl,-elf2flt=..., which turn the output into a FLAT binary.
CFLAGS ?= -O2
override CFLAGS += -Wall

memhog: memhog.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f memhog memhog.gdb

.PHONY: clean
