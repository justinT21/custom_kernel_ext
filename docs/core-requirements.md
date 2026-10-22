# What a RISC-V core needs to boot this kernel

A checklist for a custom core running `kernel.elf` (Linux 6.18.7, no MMU,
M-mode, one hart). Everything here was checked against the kernel config
(`prebuilt/config/linux.config`) and the kernel sources this tree builds.
Register layouts, the CSR bit details and a minimal device tree are in
[board/custom_rv32/readme.txt](../board/custom_rv32/readme.txt),
"Hardware requirements for a custom core". How the kernel is started and
finds its device tree is in [boot-and-device-tree.md](boot-and-device-tree.md).

## Instruction set

Required:

- [ ] **RV32I, M, A.** The kernel checks that the DT ISA string starts with
      `rv32ima` and ignores the hart otherwise. A means LR/SC and AMOs, on DRAM.
- [ ] **Zicsr, Zifencei.** `fence.i` runs at boot and whenever the kernel
      writes code (loading every program, patching itself at boot). After
      `fence.i`, instruction fetch must see the stores before it.
- [ ] **C.** The kernel and userspace are compiled with compressed
      instructions.
- [ ] **F and D.** Userspace uses the `ilp32d` ABI and the kernel doesn't
      emulate FP. No FPU means rebuilding for `ilp32`, see the readme.
- [ ] **Every FENCE encoding.** Barriers use many `fence pred,succ`
      combinations, and `cpu_relax()` executes `pause` in every spin loop.
      `pause` is `fence w,0` (`0x0100000f`). Decode any FENCE as legal
      (treating it as a full fence is fine), or spin loops trap as
      illegal instructions.
- [ ] **Division by zero doesn't trap.** It returns all ones, as the spec
      says. `cpu_relax()` divides by zero on purpose, as a stall.
- [ ] **`ecall`, `ebreak`, `mret`, `wfi`.** `ebreak` is used by `BUG()`/`WARN()`.
- [ ] **Misaligned loads and stores:** either done in hardware, or trapping
      as misaligned (cause 4/6, not access fault 5/7) with `mtval` set to
      the address. The kernel emulates the trapping case, and at boot it
      tests which one your core does.

Optional, used only if the DT lists them: V/Zve32x, Zba, Zbb, Zbkb, Zicboz,
Zacas, Zabha, Zawrs. Leave them out of the DT unless they're really
implemented, because the kernel patches itself to use them.

Not needed: S-mode, an MMU (Sv32), the `time`/`cycle` CSRs (Zicntr),
Zihpm, PMP, the H extension.

## Privilege modes

- [ ] **M-mode:** the kernel runs here.
- [ ] **U-mode:** every program runs here. `mstatus.MPP` must hold both
      00 (U) and 11 (M): the kernel tells user from kernel traps by MPP.
- S-mode isn't used.

## CSRs

All of these must be readable and writable without trapping.
Details per bit are in the readme.

| CSR | Used for |
|-----|----------|
| `mstatus` | MIE, MPIE, MPP; FS for FP save/restore (writable when F/D exist) |
| `misa` | Read at boot. 0 is allowed |
| `mie`, `mip` | MSIE (3), MTIE (7), MEIE (11). The kernel writes 0 to `mip` at boot: read-only bits must ignore the write |
| `mtvec` | Direct mode, 4-byte-aligned handler |
| `mscratch` | Every trap (0 means the trap came from the kernel) |
| `mepc` | Address of the trapping instruction |
| `mcause` | Bit 31 = interrupt, codes below |
| `mtval` | Faulting address for misaligned and access faults |
| `mhartid` | Read at boot (the a0 passed in is ignored) |
| `mvendorid`, `marchid`, `mimpid` | Read at boot to pick errata fixes. Return 0 (or your own IDs) |
| `fcsr` (`frm`, `fflags`) | FP, from userspace |

- **PMP:** optional. At boot the kernel writes `pmpaddr0` = all ones with
  `pmpcfg0` = NAPOT RWX, and skips it if those CSRs trap. Either implement
  no PMP at all, or make entry 0 support a NAPOT range covering
  everything. Otherwise all U-mode accesses fault.
- **`mcounteren`:** the kernel never writes it. U-mode `rdcycle`/`rdtime`
  trap (SIGILL) unless your reset value allows them. Nothing in the image
  uses them.
- **Unimplemented CSRs** should raise an illegal instruction exception (cause 2).

## Traps and interrupts

| `mcause` | What | Source |
|----------|------|--------|
| interrupt 3 | M software | CLINT `msip` |
| interrupt 7 | M timer | CLINT `mtime >= mtimecmp` |
| interrupt 11 | M external | PLIC |
| 1 | instruction access fault | |
| 2 | illegal instruction | |
| 3 | breakpoint | `ebreak` |
| 4 / 6 | load / store misaligned, `mtval` = address | unless done in hardware |
| 5 / 7 | load / store access fault | |
| 8 | `ecall` from U-mode | every system call |

There are no page faults without an MMU.

- [ ] **`wfi` wakes on any pending interrupt that is enabled in `mie`,
      even with `mstatus.MIE` = 0.** The idle loop executes `wfi` with
      interrupts disabled and turns them back on afterwards. A `wfi` that
      only wakes when MIE = 1 hangs at the first idle. Treating `wfi` as a
      `nop` also works.

## Harts

- [ ] **Exactly one hart may enter the kernel.** The kernel is built
      without SMP and without the "hart lottery" (`CONFIG_RISCV_BOOT_SPINWAIT`
      is off), so every hart that jumps to it runs the whole boot at the
      same time and corrupts memory. Park the other harts in your boot ROM
      (e.g. a `wfi` loop) and list only the booting hart in the DT.

## Memory

- [ ] **DRAM at 0x80000000.** The kernel is linked there and isn't
      relocatable (`CONFIG_PHYS_RAM_BASE`).
- [ ] **At least 32 MiB of DRAM;** 128 MiB is what the scripts use. Without
      an MMU each program needs one contiguous block.
- [ ] **LR/SC and AMOs work on DRAM.** The kernel also does an `sc.w` on
      every trap return to drop stale reservations.
- [ ] **The CLINT, PLIC and UART registers are uncached I/O.** The kernel
      uses 32-bit accesses on the CLINT, the PLIC and the SiFive UART, and
      the DT's `reg-io-width` on an ns16550 (8-bit on QEMU and Spike).

## Devices

Addresses are free: the kernel takes them from the DT.

- [ ] **CLINT** (`riscv,clint0` / `sifive,clint0`), the only timer driver
      for M-mode Linux. An ACLINT must use the CLINT-compatible layout
      and appear in the DT as one CLINT node:
      `msip` at +0x0, `mtimecmp` at +0x4000 (64-bit), `mtime` at +0xBFF8
      (64-bit, counting at the DT `timebase-frequency`). MTIP stays high
      while `mtime >= mtimecmp`.
- [ ] **PLIC** (`sifive,plic-1.0.0` / `riscv,plic0`), with an M-mode
      context for the hart and the `riscv,ndev` property. Needed for UART
      interrupts. The ns16550 also works polled, without one.
- [ ] **UART**: `ns16550a` (console `ttyS0`) or `sifive,uart0` (console
      `ttySIF0`). DT `/chosen/stdout-path` picks the console and
      earlycon.
- **Optional: poweroff/reboot**, a `syscon` register with
  `syscon-poweroff`/`syscon-reboot` nodes (QEMU's `sifive,test0`).
  Without it, `poweroff` and `reboot` just halt.

Nothing else is needed: no RTC, storage, network or SBI firmware.

## Boot handoff

- [ ] Start the kernel in M-mode at 0x80000000. Load `kernel.elf` by its
      program headers, or a raw image at the start of DRAM.
- [ ] `a1` = physical address of the DTB. `a0` is ignored.
- [ ] The DTB describes the memory, the one hart (`riscv,isa-base` +
      `riscv,isa-extensions`, `timebase-frequency`, the hart's interrupt
      controller), the CLINT, the PLIC, the UART and `chosen`. Or build
      it into the kernel with `CONFIG_BUILTIN_DTB`.

## Testing against Spike

Spike can describe the same core: give it your ISA string (`--isa=...`)
and your DTB (`--dtb=...`), then compare its commit log
(`trace-spike.sh`, [tracing.md](tracing.md)) with your core's retirement
log. The trace markers (`addi x0, x0, 0x5a1` / `0x5a0`) are HINTs, so a
testbench can watch for the same encodings to start and stop its own log.
