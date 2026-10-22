# Buildroot passes CC, CFLAGS and LDFLAGS on the command line (FLAT flags
# included), so add to CFLAGS with override.
CFLAGS ?= -O2
override CFLAGS += -Wall

tracemark: tracemark.c trace_marker.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f tracemark tracemark.gdb

.PHONY: clean
