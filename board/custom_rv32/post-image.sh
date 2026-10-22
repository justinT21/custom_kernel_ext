#!/bin/sh
# 1. Make kernel.elf: vmlinux with its load (physical) addresses fixed up.
#    The kernel's linker script sets each section's LMA to its offset in the
#    image (0-based) and its VMA to the link address, so loaders that place
#    segments by p_paddr (QEMU -kernel, Spike, most debuggers/loaders) would
#    put vmlinux at 0. Shifting every LMA by the entry point (= start of
#    DRAM, CONFIG_PHYS_RAM_BASE) makes LMA == VMA.
# 2. Install start-{qemu,spike}.sh, trace-{qemu,spike}.sh, trace-stats.py and
#    trace-workloads.sh next to the kernel image, with the Buildroot host
#    directory baked in so the matching emulators (and QEMU plugins) are used.

set -e

BOARD_DIR="$(dirname "$0")"

set -- "${HOST_DIR}"/bin/riscv32-*-objcopy
OBJCOPY="$1"
READELF="${OBJCOPY%objcopy}readelf"
if [ ! -x "${OBJCOPY}" ] || [ ! -x "${READELF}" ]; then
    echo "post-image: no riscv32 objcopy/readelf in ${HOST_DIR}/bin" >&2
    exit 1
fi

ENTRY="$("${READELF}" -h "${BINARIES_DIR}/vmlinux" | awk '/Entry point/ { print $4 }')"
"${OBJCOPY}" --change-section-lma "*+${ENTRY}" \
    "${BINARIES_DIR}/vmlinux" "${BINARIES_DIR}/kernel.elf"

EXT_DIR="$(cd "${BOARD_DIR}/../.." && pwd)"
for script in start-qemu.sh start-spike.sh trace-qemu.sh trace-spike.sh \
              trace-stats.py trace-workloads.sh; do
    sed -e "s|@HOST_DIR@|${HOST_DIR}|g" -e "s|@EXT_DIR@|${EXT_DIR}|g" \
        "${BOARD_DIR}/${script}.in" > "${BINARIES_DIR}/${script}"
    chmod +x "${BINARIES_DIR}/${script}"
done
