custom_rv32: no-MMU RV32IMAFDC Linux for QEMU virt, Spike and custom cores
==========================================================================

The kernel is built from Linux's nommu_virt_defconfig plus
linux-nommu.fragment (32-bit, M-mode, linked at 0x80000000). There is no
OpenSBI: the kernel is the first thing that runs, in M-mode. Userspace is
uClibc-ng + busybox as FLAT binaries, linked into the kernel as an
initramfs, so images/kernel.elf is the only image needed. It is vmlinux
with the ELF physical (load) addresses moved to DRAM by post-image.sh;
plain vmlinux has them 0-based and loaders would place it at address 0.

Build
-----

The build is out of tree. The expected directory layout is:

    siliconjackets/
        custom_kernel/        Buildroot checkout (sources only)
        custom_kernel_ext/    this BR2_EXTERNAL tree
        rv32_build/           output directory (O=), created by step 1

Run these from custom_kernel_ext/ (BR2_EXTERNAL=$PWD and the ../ paths
depend on that):

    # 1. Configure. Creates ../rv32_build with a .config generated from
    #    configs/custom_rv32_defconfig and a Makefile that forwards to
    #    Buildroot with O= set. Only needed once, or after editing
    #    the defconfig. The BR2_EXTERNAL path is saved in
    #    ../rv32_build/.br2-external.mk, so later make calls don't
    #    need it.
    make -C ../custom_kernel O=$PWD/../rv32_build \
         BR2_EXTERNAL=$PWD custom_rv32_defconfig

    # 2. Build everything.
    make -C ../rv32_build

The first build takes a while. Buildroot builds everything from source
into rv32_build/, in this order:

  1. Host tools (host/): binutils, gcc and uClibc-ng for
     riscv32-buildroot-linux-uclibc (BR2_RISCV_32, no MMU, so FLAT
     binaries), plus dtc, qemu-system-riscv32 (BR2_PACKAGE_HOST_QEMU)
     and spike (BR2_PACKAGE_HOST_RISCV_ISA_SIM). Source tarballs are
     downloaded to custom_kernel/dl/.
  2. Userspace (target/, staging/): busybox and uClibc-ng, with
     overlay/ copied on top.
  3. rootfs.cpio is packed from target/.
  4. Linux 6.18.7 (build/linux-6.18.7/): arch/riscv/configs/
     nommu_virt_defconfig, then linux-nommu.fragment merged on top. The
     result is build/linux-6.18.7/.config. Because of
     BR2_TARGET_ROOTFS_INITRAMFS, the kernel is linked last with
     rootfs.cpio embedded. BR2_LINUX_KERNEL_VMLINUX installs vmlinux in
     images/.
  5. post-image.sh: images/vmlinux -> images/kernel.elf (load addresses
     fixed up) and start-{qemu,spike}.sh.in -> images/start-*.sh.

What rv32_build/ contains:

    .config          Buildroot config (from custom_rv32_defconfig)
    Makefile         forwards `make -C rv32_build <target>` to Buildroot
    build/<pkg>/     unpacked source and build tree of each package
    host/            cross toolchain, dtc, qemu, spike (host/bin/)
    staging/         sysroot used for cross-compiling (headers, libs)
    target/          root filesystem before packing
    images/          kernel.elf, vmlinux, rootfs.cpio, start-*.sh

Common targets after the first build (all run as make -C ../rv32_build ...):

    linux-rebuild        rebuild the kernel after editing its sources in
                         build/linux-6.18.7/ (the edits are lost on a
                         linux-dirclean)
    linux-menuconfig     change the kernel config interactively. The
                         change only lasts until the next defconfig +
                         fragment merge, so copy options you want to
                         keep into linux-nommu.fragment
    linux-dirclean       delete build/linux-6.18.7/; the next make
                         unpacks it again
    menuconfig           change Buildroot options
    savedefconfig        write the Buildroot config back to
                         configs/custom_rv32_defconfig
    clean                delete everything in rv32_build/ except .config
                         (a full rebuild follows)

After editing linux-nommu.fragment, a plain `make -C ../rv32_build` is
enough. Buildroot sees that the fragment changed, merges defconfig +
fragment again and rebuilds the kernel.

The initramfs is inside vmlinux, so a userspace change (e.g. new files
in overlay/) also needs the kernel relinked. A plain
`make -C ../rv32_build` handles that too: it repacks rootfs.cpio and
relinks the kernel (rootfs-initramfs -> linux-rebuild-with-initramfs).

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


Device tree
===========

This tree has no .dts files, and the build compiles no DTB:
BR2_LINUX_KERNEL_DTS_SUPPORT is off, CONFIG_BUILTIN_DTB is not set and
build/linux-6.18.7/arch/riscv/boot/dts/dtbs-list is empty. The DTB is
generated at boot by the simulator:

  * QEMU: -M virt builds a DTB for the virt machine in memory (from -m,
    -smp, -cpu, ...). With -bios none, QEMU puts its address in a1 and
    jumps to the kernel.
  * Spike: writes a DTS from --isa, -m, -p, ..., compiles it with dtc
    (that's why start-spike.sh puts host/bin, which has dtc, on PATH)
    and places it in its boot ROM, which passes it in a1.

To see what the kernel gets:

    qemu-system-riscv32 -M virt,dumpdtb=virt.dtb -cpu rv32,mmu=off -m 128M
    dtc -I dtb -O dts virt.dtb

    spike --isa=rv32imafdc_zicntr_zihpm -m128 --dump-dts \
          ../rv32_build/images/kernel.elf

Using your own DT (e.g. one for a custom core, see the minimal example at
the end of this file):

  * Passed at run time, no rebuild needed:
        dtc -I dts -O dtb -o my.dtb my.dts      # host/bin/dtc
        ../rv32_build/images/start-qemu.sh -dtb my.dtb
        ../rv32_build/images/start-spike.sh --dtb=my.dtb
    (QEMU still emulates the virt machine, so the DT must match its
    hardware.)

  * Built by Buildroot into images/: put the source in a vendor
    subdirectory, e.g. board/custom_rv32/dts/custom/my-core.dts, and add
    to configs/custom_rv32_defconfig:
        BR2_LINUX_KERNEL_DTS_SUPPORT=y
        BR2_LINUX_KERNEL_CUSTOM_DTS_DIR="$(BR2_EXTERNAL_CUSTOM_RV32_PATH)/board/custom_rv32/dts"
    Buildroot copies the dts/ directory over the kernel's
    arch/riscv/boot/dts/, builds every .dts in it and installs
    images/my-core.dtb. Since Linux 6.12, BR2_LINUX_KERNEL_CUSTOM_DTS_PATH
    is deprecated, so don't use it. Re-run step 1 of the build (or
    `make -C ../rv32_build menuconfig`) after editing the defconfig.

  * Linked into the kernel, for a core with no boot ROM to pass a1:
    add CONFIG_BUILTIN_DTB=y and CONFIG_BUILTIN_DTB_NAME="custom/my-core"
    (relative to arch/riscv/boot/dts, without .dts) to
    linux-nommu.fragment, together with the CUSTOM_DTS_DIR setup above
    so that the file is in the kernel tree.


Boot handoff, memory layout and monitoring
==========================================

Neither simulator appends the DTB to the kernel or loads it next to
the kernel. Each one generates the DTB, puts it at its own address and
passes that address in a1. Both start at a small reset stub at
0x1000. Nothing runs at 0x0: both put a debug-module region there
(an empty placeholder on QEMU virt).

  QEMU virt, -bios none (hw/riscv/boot.c):
    0x1000      reset stub: a0 = mhartid, a1 = 0x87e00000, jump to
                0x80000000 (the kernel.elf entry)
    0x87e00000  DTB (about 5 KB): the end of DRAM minus the DTB size,
                rounded down to 2 MiB. Its address depends on -m.
                A -dtb file goes to the same place.

  Spike (riscv/sim.cc):
    0x1000      boot ROM: the reset stub, then the DTB right after it at
                0x1020. a1 = 0x1020. The DTB is in ROM, outside DRAM.

The kernel never copies the raw blob. It parses it where it is
(head.S passes a1 to setup_vm(), parse_dtb()) and reserves it in memblock
if it is in RAM, so on QEMU 0x87e00000-0x87e01fff stays "Reserved"
in /proc/iomem. unflatten_device_tree() then builds the in-kernel tree
in a ~12 KB memblock allocation. The original blob is still readable
as /sys/firmware/fdt. With CONFIG_BUILTIN_DTB, head.S ignores a1 and
uses __dtb_start, a copy linked into the kernel's init data. The kernel
copies it (unflatten_and_copy_device_tree) before init memory is freed.

Kernel size (Linux 6.18.7, this config, initramfs included):

    kernel.elf file           3.4 MiB (includes symbols, not loaded)
    loaded bytes (FileSiz)    2.8 MiB  0x80000000-0x802cb10b
    footprint (MemSiz)        2.9 MiB  0x80000000-0x802e539f (+ bss)
      code 1489K, rodata 449K, rwdata 307K, init 610K, bss 100K
    after boot                -604K: init text/data are freed, including
                              the compressed initramfs (it is unpacked
                              into the page cache as rootfs)

Check with `riscv32-buildroot-linux-uclibc-readelf -lW kernel.elf` and
`...-size -A kernel.elf` (in rv32_build/host/bin/). After a build the
numbers may differ slightly from the ones above.

RAM use at the login prompt with -m 128M (both simulators):

    Memory: 126712K/131072K available (... 4144K reserved)
      kernel image          2.9 MiB at the bottom of DRAM
      struct page array     1 MiB  (32768 pages * 32 bytes)
      DTB (QEMU only), per-cpu areas, early hash tables, the top page
    MemTotal 127532 kB, MemFree ~122800 kB, so userspace + page
    cache + slab are about 5 MB

Minimum RAM, measured on QEMU by booting with smaller -m values:

    -m      boot                                  memhog -c 60 at the prompt
    6M      kernel panic: out of memory           -
    8-16M   boots, but boot scripts and commands  -
            fail: "binfmt_flat: Unable to
            allocate RAM for process text/data"
    20M     clean                                 11.6 MiB
    24M     clean                                 15.6 MiB
    32M     clean                                 23.4 MiB
    48M     clean                                 39.0 MiB

The limit is contiguous memory, not total free memory. A FLAT binary is
"Load-to-Ram": each process gets its own copy of text + data + bss +
stack in one physically contiguous block. For busybox that's 592 KiB
(BSS End 0x8f710 + Stack 0x3e80, see flthdr below). The kernel serves
that from an order-8 (1 MiB) block, so every command needs a free 1 MiB
contiguous block. At 16M there was ~13 MB free but too fragmented.
Use at least 32 MiB. The scripts use 128 MiB.

Per-process RAM of any program in the image:

    rv32_build/host/bin/riscv32-buildroot-linux-uclibc-flthdr \
        rv32_build/target/usr/bin/<prog>
    -> about BSS End + Stack Size, as one block rounded up to a
       power-of-two order for the allocation (the unused tail is freed),
       plus whatever the program mallocs

memhog (package/memhog, source in src/memhog) measures what userspace
can still get: `memhog -l` gives the largest single block (at most
4 MiB, the kernel's order-10 limit), and `memhog -c 60` fills RAM in
60 KiB blocks. Run `dmesg -n 1` first, or each failed allocation prints
a memory dump to the console. Avoid power-of-two chunk sizes: 64 KiB +
malloc's header takes a 128 KiB block, and the fill stops with half of
RAM still free.

Watching memory from inside the guest (free, top, uptime and watch come
from busybox.fragment):

    free -k                    total/used/free/cache
    watch -n 1 free -k         the same, refreshed every second
    top                        header line is right, but the RSS column
                               is always 0 (no-MMU has no RSS)
    cat /proc/meminfo          MemFree, Slab, KernelStack, MmapCopy, ...
    vmstat 1                   free/cache once per second
    cat /proc/<pid>/status     "Mem:" line = that process's allocation
                               (no-MMU has no VmRSS)
    cat /proc/<pid>/maps       its regions (stack, mmaps)
    cat /proc/iomem            physical layout, as above
    dmesg | grep Memory:       boot-time totals
    boot with -append "earlycon memblock=debug" (QEMU) to log every
    early reservation

More busybox applets: add CONFIG_<APPLET>=y to busybox.fragment, see
docs/adding-programs.md.

From the host:

  QEMU monitor: add `-monitor tcp:127.0.0.1:4444,server,nowait` and
  connect with `nc 127.0.0.1 4444` (or press Ctrl-a c with -nographic):
    xp /10wx 0x1000              reset stub (shows the entry and DTB addr)
    xp /2wx 0x87e00000           DTB header (0xedfe0dd0 = magic, LE)
    info mtree -f                address map
    pmemsave 0x80000000 0x8000000 ram.bin    dump all of DRAM
  gdb: start-qemu.sh -s -S, then
    gdb-multiarch ../rv32_build/images/vmlinux   (Arch: riscv64-elf-gdb)
    (gdb) target remote :1234
    The build has no cross gdb. Set BR2_PACKAGE_HOST_GDB=y to get
    rv32_build/host/bin/riscv32-buildroot-linux-uclibc-gdb.

  Spike: start-spike.sh -d (or press Ctrl-c while running) opens the
  interactive debugger:
    mem 87e00000                 read a physical address
    reg 0 a1                     register of core 0
    until pc 0 80000000          run to an address
  Spike allocates guest RAM in 4 KiB host pages the first time the
  guest touches each one, so host RSS (`ps -o rss= -C spike`) roughly
  tracks the guest pages that have ever been used. QEMU's RSS includes
  its own JIT and device memory, so it isn't a useful guest number.


Hardware requirements for a custom core
=======================================

Everything below was checked against the Linux 6.18.7 sources this build
uses (arch/riscv/kernel/head.S, entry.S, traps_misaligned.c, cpufeature.c,
drivers/clocksource/timer-clint.c, drivers/irqchip/irq-sifive-plic.c,
irq-riscv-intc.c). "Required" means the kernel or userspace will not boot
or run without it. docs/core-requirements.md has the same requirements
as a checklist.

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

  * Every FENCE encoding. Barriers use many pred/succ combinations, and
    cpu_relax() runs `pause` (= fence w,0, 0x0100000f) in every spin
    loop, on every core except MIPS P8700. Decode any FENCE as legal.
  * Division by zero must not trap (it returns all ones, per the spec):
    cpu_relax() also divides by zero on purpose, as a stall.

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

Other instructions: mret and wfi. ecall/ebreak must trap as described
above. The idle loop runs wfi with mstatus.MIE = 0 and enables interrupts
after it, so wfi must wake when (mip & mie) != 0 regardless of MIE, as
the spec requires. A wfi that waits for MIE = 1 hangs at the first idle.
wfi may also be a nop.

Harts: the kernel is built without SMP and without
CONFIG_RISCV_BOOT_SPINWAIT, so it has no "hart lottery". Every hart that
jumps to it runs the whole boot at once. Only one hart may enter the
kernel; park the others in the boot ROM and list only that hart in the DT.

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
