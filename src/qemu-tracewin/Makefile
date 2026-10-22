# Built as a host package: Buildroot passes the host CC and CFLAGS (which
# include -I$(HOST_DIR)/include, where qemu-plugin.h is) and a PKG_CONFIG
# that finds the host glib.
CFLAGS ?= -O2
override CFLAGS += -Wall -fPIC $(shell $(PKG_CONFIG) --cflags glib-2.0)

libtracewin.so: tracewin.c
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $<

clean:
	rm -f libtracewin.so

.PHONY: clean
