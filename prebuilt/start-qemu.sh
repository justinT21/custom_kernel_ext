#!/bin/sh
# Boot the no-MMU M-mode kernel on QEMU virt. There is no firmware: QEMU
# jumps straight to the kernel in M-mode with a0=hartid, a1=dtb.
# Extra arguments are passed through to QEMU (e.g. -s -S for gdb).
# Override QEMU to use another qemu-system-riscv32 binary.

IMAGE_DIR="$(dirname "$0")"
: "${QEMU:=qemu-system-riscv32}"

exec "${QEMU}" \
    -M virt \
    -cpu rv32,mmu=off \
    -bios none \
    -m 128M \
    -nographic \
    -kernel "${IMAGE_DIR}/kernel.elf" \
    "$@"
