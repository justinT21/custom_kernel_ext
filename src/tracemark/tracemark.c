/*
 * tracemark on|off: execute a trace marker, to start or stop QEMU/Spike
 * tracing from the shell:
 *
 *   tracemark on; memhog -l; tracemark off
 *
 * The trace covers everything in between, including the shell starting
 * the command and the kernel. For a tighter window, put TRACE_ON() and
 * TRACE_OFF() from <trace_marker.h> in your own code.
 */
#include <stdio.h>
#include <string.h>

#include "trace_marker.h"

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "on")) {
		TRACE_ON();
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "off")) {
		TRACE_OFF();
		return 0;
	}
	fprintf(stderr, "usage: %s on|off\n", argv[0]);
	return 1;
}
