#!/bin/sh
# Copy a finished build into prebuilt/, so the kernel can be booted with
# the system's QEMU or Spike without building anything (README.md,
# "Prebuilt image"). Run it by hand after a build you want to share;
# Buildroot never runs it, so builds don't change tracked files.
#
#   board/custom_rv32/update-prebuilt.sh [BUILD_DIR]
#
# BUILD_DIR is the Buildroot output directory (default: ../rv32_build).

set -e

EXT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD_DIR="${EXT_DIR}/board/custom_rv32"
BUILD_DIR="$(cd "${1:-${EXT_DIR}/../rv32_build}" && pwd)"
OUT="${EXT_DIR}/prebuilt"
IMAGES="${BUILD_DIR}/images"

if [ ! -e "${IMAGES}/kernel.elf" ]; then
    echo "update-prebuilt: no ${IMAGES}/kernel.elf, build first" >&2
    exit 1
fi

mkdir -p "${OUT}/config"
cp "${IMAGES}/kernel.elf" "${OUT}/kernel.elf"

# Same start scripts as images/, minus the line that puts Buildroot's
# host/bin first in PATH, so the system's emulators are used
for script in start-qemu.sh start-spike.sh; do
    sed '/@HOST_DIR@/d' "${BOARD_DIR}/${script}.in" > "${OUT}/${script}"
    chmod +x "${OUT}/${script}"
done

# The resolved configs (the tree only has the fragments)
cp "${BUILD_DIR}/build/linux-"*/.config "${OUT}/config/linux.config"
cp "${BUILD_DIR}/build/busybox-"*/.config "${OUT}/config/busybox.config"

version() { basename "$1" | sed 's/^[a-z-]*-//'; }
set -- "${BUILD_DIR}"/build/host-riscv-isa-sim-*
spike=$1
set -- "${BUILD_DIR}"/host/bin/riscv32-*-gcc
gcc=$1
{
    echo "kernel.elf for custom_rv32 (RV32IMAFDC, no MMU, M-mode, busybox initramfs)"
    echo
    echo "custom_rv32:  $(git -C "${EXT_DIR}" describe --always --dirty 2>/dev/null || echo unknown)"
    echo "Built:        $(date -r "${IMAGES}/kernel.elf" '+%Y-%m-%d %H:%M')"
    echo "Linux:        $(version "${BUILD_DIR}"/build/linux-[0-9]*)"
    echo "busybox:      $(version "${BUILD_DIR}"/build/busybox-*)"
    echo "uClibc-ng:    $(version "${BUILD_DIR}"/build/uclibc-[0-9]*)"
    echo "gcc:          $("${gcc}" -dumpversion)"
    echo "Built with:   QEMU $(version "${BUILD_DIR}"/build/host-qemu-[0-9]*), Spike $(basename "${spike}" | sed 's/.*-\(.......\).*/\1/') (patched)"
    echo
    echo "sha256:"
    (cd "${OUT}" && sha256sum kernel.elf config/linux.config config/busybox.config)
} > "${OUT}/BUILD-INFO.txt"

cat "${OUT}/BUILD-INFO.txt"
echo "update-prebuilt: wrote ${OUT}"
