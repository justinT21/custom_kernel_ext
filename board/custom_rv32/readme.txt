custom_rv32: no-MMU RV32IMAFDC Linux for QEMU virt, Spike and custom cores
==========================================================================

The kernel is built from Linux's nommu_virt_defconfig plus
linux-nommu.fragment (32-bit, M-mode, linked at 0x80000000). There is no
OpenSBI: the kernel is the first thing that runs, in M-mode. Userspace is
uClibc-ng + busybox as FLAT binaries, linked into the kernel as an
initramfs, so images/kernel.elf is the only image needed. It is vmlinux
with the ELF physical (load) addresses moved to DRAM by post-image.sh;
plain vmlinux has them 0-based and loaders would place it at address 0.

Build (out of tree, from the buildroot checkout):

    make -C ../custom_kernel O=$PWD/../rv32_build \
         BR2_EXTERNAL=$PWD custom_rv32_defconfig
    make -C ../rv32_build

Run:

    ../rv32_build/images/start-qemu.sh
    ../rv32_build/images/start-spike.sh

Anything after the script name goes straight to qemu/spike, e.g.
`start-qemu.sh -s -S` to wait for gdb, or `start-spike.sh -d` for
Spike's interactive debugger.

Both scripts boot the same kernel.elf. Every device address except DRAM
comes from the DTB the simulator passes in a1:

                  QEMU virt / Spike
    UART          ns16550 @ 0x10000000   (ttyS0)
    CLINT         0x02000000             (M-mode timer/IPI)
    PLIC          0x0c000000
    DRAM          0x80000000             (CONFIG_PHYS_RAM_BASE, fixed)

The kernel has both the ns16550 driver (ttyS0) and the SiFive UART driver
(ttySIF0). The DT's /chosen/stdout-path picks the console and the
earlycon, so no console= is needed. With no bootargs from the loader
(e.g. QEMU without -append), the command line is CONFIG_CMDLINE
("earlycon"). Spike always passes its own bootargs.


Hardware requirements for a custom core
=======================================

Everything below was checked against the Linux 6.18.7 sources this build
uses (arch/riscv/kernel/head.S, entry.S, traps_misaligned.c, cpufeature.c,
drivers/clocksource/timer-clint.c, drivers/irqchip/irq-sifive-plic.c,
irq-riscv-intc.c). "Required" means the kernel or userspace will not boot
or run without it.

Unprivileged ISA
----------------

Required:
  * RV32I, M, A. The kernel is compiled with -march=rv32ima... and refuses
    any hart whose DT ISA string does not start with "rv32ima". A needs
    both LR/SC and AMOs on DRAM.
  * Zicsr, Zifencei. fence.i is executed at boot and whenever the kernel
    writes code (e.g. loading FLAT binaries).
  * C, because the kernel and userspace are built with it
    (BR2_RISCV_ISA_RVC / CONFIG_RISCV_ISA_C). Drop both together to build
    without it.
  * F and D, because userspace is built for the ilp32d ABI and the kernel
    does not emulate FP. The kernel only supports F together with D (it
    ignores F-without-D). For a core without an FPU, switch Buildroot to
    the ilp32 ABI (unset RVF/RVD) and set "# CONFIG_FPU is not set".

Optional (compiled in, but used only when the DT advertises them, via
runtime patching): V/Zve32x, Zba, Zbb, Zbkb, Zicboz, Zacas, Zabha, Zawrs.

Not needed: Zicntr/Zihpm. The kernel reads time from the CLINT's mtime
over MMIO, not the time CSR. See mcounteren below for userspace.

Privilege modes
---------------

  * M-mode: required. The kernel runs here.
  * U-mode: required. User processes run in U-mode. The kernel decides
    whether a trap came from user or kernel space by checking
    mstatus.MPP == 0, so MPP must be able to hold both 00 (U) and 11 (M).
  * S-mode: not needed (any S-mode PLIC contexts are disabled at boot).

M-mode CSRs
-----------

Must be implemented (reads/writes must not trap):

  mstatus   MIE, MPIE and MPP (U and M values) must work. FS must be
            writable if F/D are present, because the kernel uses FS for
            lazy FP save and restore (VS too if V is enabled). MPRV, SUM
            and MXR are not used.
  misa      Read in reset_regs to decide whether to zero the FP/V
            registers. Reading 0 is allowed, but then those registers are
            not cleared at boot.
  mie/mip   Uses MSIE (bit 3), MTIE (bit 7) and MEIE (bit 11). At boot the
            kernel writes 0 to mip, so read-only bits must ignore writes
            rather than trap.
  mtvec     Direct mode only (MODE=0); handler addresses are 4-byte
            aligned. Vectored mode is not used.
  mscratch  Used on every trap to tell user entry from kernel entry
            (0 = trap from kernel).
  mepc      Must hold the address of the trapping instruction. The kernel
            adds 4 after ecall.
  mcause    Interrupt bit 31 (RV32); codes are listed below.
  mtval     Must hold the faulting address for misaligned and access
            faults: the misaligned-access emulation reads it as the
            target address. It is not needed for illegal instruction
            (the kernel re-reads the instruction from mepc).
  mhartid   Read at boot. The hart ID passed in a0 is ignored.
  mvendorid, marchid, mimpid
            Read at boot to select vendor errata. Return 0 (or your own
            IDs) so that no SiFive/T-Head/Andes errata patching is used.

PMP: optional. At boot the kernel tries to program pmpaddr0 = all ones
with pmpcfg0 = NAPOT|R|W|X, i.e. one entry covering all of memory, so
that U-mode can access memory. If the PMP CSRs trap, the kernel skips
this. So either implement no PMP at all, or make entry 0 support NAPOT
covering the full address space. With some entries implemented but this
write unsupported, every U-mode access faults (per the privileged spec,
no matching entry means access is denied).

mcounteren: never written by the kernel, so U-mode rdcycle/rdtime/rdinstret
trap as illegal instructions (SIGILL). Busybox/uClibc do not use them.
If your own programs need them, reset mcounteren to the bits you want
readable from U-mode.

menvcfg: only touched if the DT advertises an extension that needs it
(e.g. Zicboz, pointer masking). Not needed for the base config.

Traps the kernel must receive (mcause):
  Interrupts: 3 M-software (CLINT msip), 7 M-timer (CLINT mtimecmp),
              11 M-external (PLIC)
  Exceptions: 2 illegal instruction (also used for lazy V enable)
              3 breakpoint (BUG()/WARN() use ebreak)
              4/6 load/store address misaligned, with mtval = address
                  (or support misaligned access in hardware; the kernel
                  probes which at boot). Must be reported as misaligned
                  (4/6), not as an access fault (5/7).
              5/7 load/store access fault, 1 instruction access fault
              8 ecall from U-mode (system calls)
  There are no page faults (12/13/15) without an MMU.

Other instructions: mret and wfi (used by the idle loop; may be a nop).
ecall/ebreak must trap as described above.

CLINT / ACLINT
--------------

The M-mode kernel's only timer/IPI driver is timer-clint.c, which matches
only compatible = "riscv,clint0" or "sifive,clint0". Linux 6.18 has no M-mode
driver for separate ACLINT MTIMER/MSWI nodes. An ACLINT therefore has to
use the SiFive-CLINT-compatible layout (described by the ACLINT spec)
and appear in the DT as a single CLINT node:

  base + 0x0000 + 4*hart   msip      (ACLINT MSWI)   bit 0 raises MSIP
  base + 0x4000 + 8*hart   mtimecmp  (ACLINT MTIMER) 64-bit
  base + 0xBFF8            mtime     (ACLINT MTIMER) 64-bit, free-running

  * 32-bit accesses must work on every register. On RV32 the kernel reads
    mtime as hi/lo/hi and writes mtimecmp low word first, then high.
  * mtime counts at the DT /cpus "timebase-frequency".
  * MTIP must be level: asserted while mtime >= mtimecmp, cleared by
    writing a later mtimecmp.
  * The CLINT node's interrupts-extended must list the hart intc with
    both 3 (M-soft) and 7 (M-timer); the driver fails without both.
  * The base address is taken from the DT; 0x02000000 is just convention.

PLIC
----

Driver: irq-sifive-plic.c, compatible "sifive,plic-1.0.0" or
"riscv,plic0". Register layout (32-bit accesses):

  base + 0x000000 + 4*src        priority (the kernel writes 1)
  base + 0x002000 + 0x80*ctx     enable bits, one per source
  base + 0x200000 + 0x1000*ctx   threshold (kernel writes 0)
  base + 0x200004 + 0x1000*ctx   claim/complete

  * Needs an M-mode external-interrupt context per hart. In the DT the
    PLIC's interrupts-extended entries are contexts in order. The kernel
    uses the ones whose parent hwirq is 11 (MEIP) and disables the S-mode
    (9) ones. A core with only M-mode needs just <&cpu0_intc 11>.
  * The "riscv,ndev" property (number of sources) is required.
  * Source 0 is reserved; device interrupts start at 1.
  * Claim/complete behave as in the PLIC spec; level-triggered sources
    are expected.

UART / console
--------------

Either UART type works at any address; /chosen/stdout-path in the DT
selects the console and earlycon.

SiFive UART (driver drivers/tty/serial/sifive.c, console ttySIF0)
  compatible = "sifive,uart0". 32-bit registers:

    0x00 txdata  [7:0] data, bit 31 = FIFO full (write ignored while full)
    0x04 rxdata  [7:0] data, bit 31 = FIFO empty
    0x08 txctrl  bit 0 txen, bit 1 nstop, [18:16] txcnt watermark
    0x0c rxctrl  bit 0 rxen, [18:16] rxcnt watermark
    0x10 ie      bit 0 txwm, bit 1 rxwm
    0x14 ip      bit 0 txwm (tx level < txcnt), bit 1 rxwm (rx level > rxcnt)
    0x18 div     [15:0], baud = clk / (div + 1); the kernel programs 115200

  Probe fails unless the DT node has ALL of:
    * interrupts     (a PLIC source; the driver uses txwm/rxwm interrupts)
    * clocks         (a phandle; a "fixed-clock" node is enough, and its
                      clock-frequency is used to compute div)
    * an aliases entry, e.g. aliases { serial0 = &uart0; }
  Earlycon only needs reg (it polls txdata bit 31).

ns16550 (drivers/tty/serial/8250, console ttyS0)
  compatible = "ns16550a", with clock-frequency, reg-shift and
  reg-io-width (QEMU virt and Spike use 8-bit registers: reg-shift = 0,
  reg-io-width = 1). Without an "interrupts" property it is polled.

Memory and boot handoff
-----------------------

  * DRAM must start at CONFIG_PHYS_RAM_BASE (0x80000000). The kernel is
    linked and loaded there and is not relocatable. Use at least 32 MiB;
    the scripts use 128 MiB.
  * Load kernel.elf by its program headers (p_paddr == p_vaddr ==
    0x80000000), or load images/Image (vmlinux as a raw binary, built with
    BR2_LINUX_KERNEL_IMAGE) at the start of DRAM.
  * Entry: jump to the start of DRAM in M-mode with a1 = physical address
    of the DTB. a0 is ignored (mhartid is read). Interrupts may be in any
    state; the kernel clears mie/mip.
  * Without a boot ROM that supplies a DTB, set CONFIG_BUILTIN_DTB=y and
    CONFIG_BUILTIN_DTB_NAME to a dts under arch/riscv/boot/dts/.

Minimal device tree for one hart (adjust addresses and frequencies):

  /dts-v1/;
  / {
      #address-cells = <1>; #size-cells = <1>;
      compatible = "custom,rv32-nommu";
      aliases { serial0 = &uart0; };       /* required by the SiFive driver */
      chosen { stdout-path = &uart0; };    /* console + earlycon */
      memory@80000000 {
          device_type = "memory"; reg = <0x80000000 0x08000000>;
      };
      cpus {
          #address-cells = <1>; #size-cells = <0>;
          timebase-frequency = <10000000>;
          cpu0: cpu@0 {
              device_type = "cpu"; compatible = "riscv"; reg = <0>;
              riscv,isa-base = "rv32i";
              riscv,isa-extensions = "i", "m", "a", "f", "d", "c",
                                     "zicsr", "zifencei";
              cpu0_intc: interrupt-controller {
                  compatible = "riscv,cpu-intc";
                  interrupt-controller; #interrupt-cells = <1>;
              };
          };
      };
      uart_clk: uart-clock {
          compatible = "fixed-clock"; #clock-cells = <0>;
          clock-frequency = <50000000>;    /* your UART input clock */
      };
      soc {
          #address-cells = <1>; #size-cells = <1>;
          compatible = "simple-bus"; ranges;
          clint@2000000 {
              compatible = "riscv,clint0";
              reg = <0x02000000 0x10000>;
              interrupts-extended = <&cpu0_intc 3>, <&cpu0_intc 7>;
          };
          plic: interrupt-controller@c000000 {
              compatible = "riscv,plic0";
              reg = <0x0c000000 0x4000000>;
              interrupt-controller; #interrupt-cells = <1>;
              #address-cells = <0>;
              riscv,ndev = <31>;
              interrupts-extended = <&cpu0_intc 11>;
          };
          uart0: serial@10010000 {
              compatible = "sifive,uart0";
              reg = <0x10010000 0x1000>;
              clocks = <&uart_clk>;
              interrupt-parent = <&plic>; interrupts = <1>;
          };
          /* or, for a ns16550 instead:
           * uart0: serial@10000000 {
           *     compatible = "ns16550a"; reg = <0x10000000 0x100>;
           *     clock-frequency = <10000000>;
           *     reg-shift = <0>; reg-io-width = <1>;
           *     interrupt-parent = <&plic>; interrupts = <1>;
           * };
           */
      };
  };

Quick test against QEMU's view of the same requirements: dump its DTB with
`qemu-system-riscv32 -M virt,dumpdtb=virt.dtb -cpu rv32,mmu=off` and
`dtc -I dtb virt.dtb`.
