#!/bin/sh
# Boot the no-MMU M-mode kernel on Spike. Spike's boot ROM jumps to the ELF
# entry point in M-mode with a0=hartid, a1=dtb; the console is Spike's
# ns16550. Extra arguments are passed through to Spike (e.g. -d, -l).
# Override SPIKE to use another spike binary.

IMAGE_DIR="$(dirname "$0")"
: "${SPIKE:=spike}"

exec "${SPIKE}" \
    --isa=rv32imafdc_zicntr_zihpm \
    -m128 \
    "$@" \
    "${IMAGE_DIR}/kernel.elf"
