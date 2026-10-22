#!/bin/sh
# Give FLAT binaries a usable stack. Without an MMU the stack can't grow,
# and elf2flt's default is 4 KiB: only packages that set
# <PKG>_FLAT_STACKSIZE get more. Raise every binary still at that default
# to FLAT_MIN_STACK (sqlite3 segfaults with 4 KiB). Each process pays for
# its stack in RAM.

set -e

FLAT_MIN_STACK=32768

set -- "${HOST_DIR}"/bin/riscv32-*-flthdr
FLTHDR="$1"

find "${TARGET_DIR}" -type f -perm -u+x | while read -r f; do
    [ "$(head -c 4 "$f")" = bFLT ] || continue
    stack=$("${FLTHDR}" "$f" | awk '/Stack Size/ { print $3 }')
    if [ "$((stack))" -le 4096 ]; then
        "${FLTHDR}" -s "${FLAT_MIN_STACK}" "$f"
        echo "post-build: ${f#"${TARGET_DIR}"}: stack ${stack} -> ${FLAT_MIN_STACK}"
    fi
done
