/*
 * memhog: measure how much RAM user programs can get on the no-MMU kernel.
 *
 * Without an MMU every allocation is physically contiguous, so two numbers
 * matter: the largest single block (limited by fragmentation) and the total
 * that can be allocated in small blocks. memhog reports both, touching every
 * byte so the memory is really in use.
 *
 *   memhog [-c chunk_kib] [-n max_mib] [-s hold_seconds] [-l]
 *     -c  allocation size for the fill phase (default 64 KiB)
 *     -n  stop after this many MiB (default: until malloc fails)
 *     -s  keep the memory for this long before exiting (default 0)
 *     -l  find the largest single block instead of filling
 *
 * Each failed allocation makes the kernel print an error and a memory dump
 * to the console, and the first request over 4 MiB per boot also prints a
 * WARNING with a backtrace (expected; see below). Run `dmesg -n 1` first to keep them off the console
 * (they stay in dmesg).
 *
 * Kernel behavior this shows (mm/nommu.c, do_mmap_private):
 *   - One allocation is one alloc_pages_exact() call, limited to order 10,
 *     so no single block can exceed 4 MiB (4095 KiB after malloc's header).
 *   - The kernel takes a power-of-two block and frees the unused tail
 *     (nr_trim_pages). With -c 64, 64 KiB + header rounds up to 128 KiB,
 *     and the freed tails are too small for the next request, so the fill
 *     stops with about half of RAM still free. -c 60 fills almost all of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long meminfo_kib(const char *key)
{
	char line[128];
	size_t n = strlen(key);
	long val = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ':') {
			val = atol(line + n + 1);
			break;
		}
	fclose(f);
	return val;
}

/* Binary search for the largest malloc() that succeeds, in KiB. */
static long largest_block_kib(void)
{
	long lo = 0, hi = meminfo_kib("MemTotal");

	while (lo < hi) {
		long mid = (lo + hi + 1) / 2;
		void *p = malloc((size_t)mid * 1024);

		if (p) {
			/* Use the block, or gcc drops the malloc/free pair
			 * and assumes malloc succeeded. */
			*(volatile char *)p = 0;
			free(p);
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

int main(int argc, char **argv)
{
	long chunk_kib = 64, max_mib = 0, hold = 0, total_kib = 0;
	int only_largest = 0, opt;

	while ((opt = getopt(argc, argv, "c:n:s:l")) != -1) {
		switch (opt) {
		case 'c': chunk_kib = atol(optarg); break;
		case 'n': max_mib = atol(optarg); break;
		case 's': hold = atol(optarg); break;
		case 'l': only_largest = 1; break;
		default:
			fprintf(stderr, "usage: %s [-c chunk_kib] [-n max_mib] [-s hold_seconds] [-l]\n",
				argv[0]);
			return 1;
		}
	}
	if (chunk_kib <= 0)
		chunk_kib = 64;

	printf("MemTotal %ld KiB, MemFree %ld KiB\n",
	       meminfo_kib("MemTotal"), meminfo_kib("MemFree"));
	if (only_largest) {
		printf("largest single block: %ld KiB\n", largest_block_kib());
		return 0;
	}

	for (;;) {
		char *p;

		if (max_mib && total_kib >= max_mib * 1024)
			break;
		p = malloc((size_t)chunk_kib * 1024);
		if (!p)
			break;
		memset(p, 0xa5, (size_t)chunk_kib * 1024);
		total_kib += chunk_kib;
		if (total_kib % 1024 < chunk_kib)
			printf("  %6ld KiB allocated, MemFree %ld KiB\n",
			       total_kib, meminfo_kib("MemFree"));
	}
	printf("allocated %ld KiB in %ld KiB blocks, MemFree now %ld KiB\n",
	       total_kib, chunk_kib, meminfo_kib("MemFree"));
	fflush(stdout);
	if (hold)
		sleep(hold);
	return 0;
}
