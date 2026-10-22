/* SPDX-License-Identifier: MIT */
/*
 * Trace markers (custom_rv32, see docs/tracing.md).
 *
 * Both are RISC-V HINT instructions: architectural no-ops on any core.
 * QEMU's tracewin plugin and Spike (SPIKE_TRACE_GATE=1 --log-commits)
 * trace everything executed between TRACE_ON() and TRACE_OFF(), on the
 * whole system (other processes and the kernel included).
 *
 *   TRACE_ON():  addi x0, x0, 0x5a1   (0x5a100013)
 *   TRACE_OFF(): addi x0, x0, 0x5a0   (0x5a000013)
 */
#ifndef TRACE_MARKER_H
#define TRACE_MARKER_H

/* norvc keeps them 32-bit, so the encodings above are exact */
#define TRACE_ON()  __asm__ volatile(".option push\n.option norvc\n" \
				     "addi x0, x0, 0x5a1\n.option pop" ::: "memory")
#define TRACE_OFF() __asm__ volatile(".option push\n.option norvc\n" \
				     "addi x0, x0, 0x5a0\n.option pop" ::: "memory")

#endif
