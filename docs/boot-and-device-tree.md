# Boot flow and the device tree

How `kernel.elf` from this tree gets started and gets its device tree (DTB)
on QEMU, on Spike and on a physical core. Everything here was checked
against the sources this build uses: Linux 6.18.7, QEMU 11.0.0 and Spike
(riscv-isa-sim) `00dfa28c`, all under `rv32_build/build/`. The rest was
seen by booting `rv32_build/images/kernel.elf` with `-m 128M`.

For the kernel's hardware requirements (CSRs, CLINT, PLIC, UART) and a
minimal DTS for a custom core, see
[board/custom_rv32/readme.txt](../board/custom_rv32/readme.txt).

## 1. What the kernel needs at entry

- M-mode, PC = 0x80000000 (`CONFIG_PHYS_RAM_BASE`, the start of
  `kernel.elf`)
- `a0` = hart ID (ignored; the kernel reads `mhartid`)
- `a1` = physical address of a DTB, 8-byte aligned (libfdt rejects
  anything else)

Whatever runs at reset has to set this up. The kernel isn't the first
code to run, even though there is no OpenSBI or firmware.

## 2. How the emulators do it

Neither one appends the DTB to the kernel. Each generates a DTB, puts it at
its own address and passes that address in `a1`. Both start at a tiny
reset stub at 0x1000.

### QEMU `virt`, `-bios none` (`hw/riscv/boot.c`)

The reset stub is at 0x1000 (`VIRT_MROM`). Read from the QEMU monitor
(`xp /10wx 0x1000`):

```
00001000: 0x00000297 0x02828613 0xf1402573 0x0202a583
00001010: 0x0182a283 0x00028067 0x80000000 0x00000000
00001020: 0x87e00000 0x00000000
```

```asm
1:  auipc t0, 0              # t0 = 0x1000
    addi  a2, t0, 40         # a2 = fw_dynamic info (unused by Linux)
    csrr  a0, mhartid
    lw    a1, 32(t0)         # a1 = 0x87e00000  (DTB)
    lw    t0, 24(t0)         # t0 = 0x80000000  (kernel entry)
    jr    t0
    .word 0x80000000, 0      # start address
    .word 0x87e00000, 0      # fdt address
```

The DTB address is `ALIGN_DOWN(end_of_DRAM - fdt_size, 2 MiB)`
(`riscv_compute_fdt_addr`), capped at 3 GiB on RV32. With `-m 128M` that's
0x87e00000. It moves if you change `-m`. A file given with `-dtb` goes to
the same place.

### Spike (`riscv/sim.cc`, `sim_t::set_rom`)

The boot ROM is at 0x1000 (`DEFAULT_RSTVEC`). It holds an 8-word stub,
then the DTB right after it at 0x1020, padded to 4 KiB:

```asm
    auipc t0, 0              # t0 = 0x1000
    addi  a1, t0, 32         # a1 = 0x1020  (DTB, right after the stub)
    csrr  a0, mhartid
    lw    t0, 24(t0)         # t0 = ELF entry point
    jr    t0
    .word 0
    .word <entry lo>, <entry hi>
<DTB>
```

So on Spike the DTB is in ROM, outside DRAM.

### Comparison

|                    | QEMU virt                   | Spike                       |
|--------------------|-----------------------------|-----------------------------|
| Reset PC           | 0x1000 (MROM)               | 0x1000 (boot ROM)           |
| DTB location       | 0x87e00000 (top of DRAM)    | 0x1020 (inside the ROM)     |
| DTB size           | ~5 KB                       | ~1.6 KB                     |
| Kernel loaded by   | `-kernel` (ELF program headers) | ELF loader (program headers) |
| What's at 0x0      | `VIRT_DEBUG` placeholder    | debug module (0x0-0xfff)    |

## 3. What the kernel does with the DTB

1. `head.S` moves `a1` into `a0` and calls `setup_vm()`, which saves it
   as `dtb_early_pa`/`dtb_early_va` (no MMU, so VA == PA). With
   `CONFIG_BUILTIN_DTB`, `head.S` ignores `a1` and passes `__dtb_start`
   instead.
2. `parse_dtb()` → `early_init_dt_scan()` reads `/chosen` (bootargs,
   stdout-path), `/memory` and the CPU nodes directly from the blob.
3. `paging_init()` → `setup_bootmem()` reserves the kernel image and the
   blob (`memblock_reserve(dtb_early_pa, fdt_totalsize)`), so the blob is
   never overwritten. On QEMU this shows up as
   `87e00000-87e01fff : Reserved` in `/proc/iomem`. On Spike the blob is
   outside RAM, but the reservation still appears as
   `00001000-00001fff : Reserved`.
4. `unflatten_device_tree()` builds the in-kernel tree in a ~12 KB memblock
   allocation. The original blob stays where it is and can be read as
   `/sys/firmware/fdt`. With `CONFIG_BUILTIN_DTB`, the kernel calls
   `unflatten_and_copy_device_tree()` instead, because the built-in copy is
   in init memory that gets freed.

Seen with `-append "earlycon memblock=debug"` on QEMU:

```
__memblock_reserve: [0x80000000-0x802e539f] ... paging_init   <- kernel image
__memblock_reserve: [0x87e00000-0x87e013d8] ... paging_init   <- DTB (0x13d9 bytes)
memblock_alloc_try_nid: 12012 bytes ...                       <- unflattened tree
```

## 4. On a physical core

Real hardware has to do the same things the emulators fake.

### 4.1 Reset: a boot ROM

Put a small ROM at your core's reset PC. Spike's design copies directly to
hardware: a stub with the DTB in the same ROM.

```asm
    .globl _start
_start:
    csrr  a0, mhartid
    bnez  a0, park            # only hart 0 boots (CONFIG_SMP is off)
    la    a1, dtb             # PC-relative, works from ROM
    li    t0, 0x80000000
    jr    t0
park:
    wfi
    j     park

    .balign 8                 # FDT must be 8-byte aligned
dtb:
    .incbin "my-core.dtb"
```

Build: `dtc -I dts -O dtb -o my-core.dtb my-core.dts`, then assemble and
link with `-nostdlib -Ttext=<reset PC>`. For a ROM init file, convert with
`objcopy -O binary` or `-O verilog` (for `$readmemh`).

- **Normal loads must reach the ROM.** The kernel reads the DTB in place
  and keeps using it, so instruction fetch alone isn't enough.
- **Keep the ROM out of the DT's `memory` node.**
- **Park every other hart.** Only hart 0 may enter the kernel.

### 4.2 Getting the kernel into DRAM

DRAM is empty at reset. Choose one:

| Method               | How                                                                                         | Good for                |
|----------------------|---------------------------------------------------------------------------------------------|-------------------------|
| Preload in sim       | `objcopy -O verilog kernel.elf` → `$readmemh` into the RAM model                            | RTL sim (Verilator/VCS) |
| Debugger             | JTAG + OpenOCD + gdb: halt, `load kernel.elf`, `set $a1 = <dtb>`, `set $pc = 0x80000000`, continue | First bring-up          |
| FPGA init            | Kernel in BRAM init contents, or a host writes it over UART/PCIe before releasing reset       | FPGA prototypes         |
| Loader in ROM        | ROM copies a raw `Image` from SPI flash/SD to 0x80000000, runs `fence.i`, then jumps         | Standalone boards/chips |
| Bootloader chain     | ROM → SPL (trains DDR) → U-Boot → kernel. U-Boot's `bootm` sets `a0`/`a1`                   | Boards with real DDR    |

A raw `Image` (`objcopy -O binary vmlinux Image`, or
`BR2_LINUX_KERNEL_IMAGE=y`) is easier to copy than an ELF.

- **DRAM may need init first.** DDR has to be trained before use (that's
  the SPL's job). SRAM, HyperRAM/PSRAM and FPGA memory may not need
  it.
- **Plan for at least 32 MiB of RAM.** On QEMU, 6 MiB panics, 8-16 MiB
  boots but programs fail to start (each busybox process needs a free,
  contiguous 1 MiB block), and 20 MiB is the smallest clean boot. That's
  more than on-chip SRAM usually gives you. See "Minimum RAM" in
  `board/custom_rv32/readme.txt`.
- **Run `fence.i` before jumping.** If the loader copied the kernel through
  the D-side and the core has an I-cache, it must execute `fence.i` first.

### 4.3 Where to put the DTB

1. **In the boot ROM, like Spike.** The kernel stays generic and the ROM
   describes the chip. Rocket/Chipyard does this, generating the DTS from
   the SoC config.
2. **Copied into DRAM by a loader, like QEMU or U-Boot.** Place it 8-byte
   aligned, past the kernel's bss (0x802e53a0 here) and out of the way of
   early allocations (QEMU uses the top of RAM). The kernel reserves it
   automatically.
3. **Built into the kernel (`CONFIG_BUILTIN_DTB=y`,
   `CONFIG_BUILTIN_DTB_NAME="<vendor>/<name>"`).** The ROM only has to jump
   to 0x80000000, but the kernel then only fits that one chip. The Kendryte
   K210 (no-MMU RV64, 8 MB SRAM) boots Linux this way: its boot ROM copies
   the kernel from SPI flash into SRAM.

### 4.4 The DTB must match the silicon

A wrong value here usually shows up as a silent hang or garbage on the
serial port.

- **`timebase-frequency`:** the real clock that drives `mtime`. If it's
  wrong, delays are off and boot can stall at the clocksource.
- **UART clock:** `clock-frequency` (ns16550) or the `fixed-clock` (SiFive
  UART) is the real UART input clock. If it's wrong, the baud divisor is
  wrong.
- **`memory` node:** the real DRAM size.
- **ISA string:** only extensions the core really implements. Optional
  ones like Zbb are switched on from this string.
- **CLINT/PLIC addresses and `riscv,ndev`:** must match your RTL.

Bring up with earlycon on the UART first. If a few characters come out,
the jump, `a1`, DTB parsing and UART are all working.

## 5. Why the reset vector isn't at 0

The RISC-V privileged spec leaves the reset address
implementation-defined. 0x1000 is the convention from SiFive/Rocket, which
QEMU and Spike follow, and it has three reasons behind it:

1. **The debug module lives at 0x0-0xfff.** When a debugger halts the
   hart, the debug ROM runs without being allowed to clobber any GPR, so
   it addresses memory relative to `x0`. A 12-bit signed offset from `x0`
   reaches only 0x000-0x7ff and the top 2 KiB, which forces the debug
   module near 0. From Spike's `debug_rom/debug_rom.S`:

   ```asm
   sw   s0, DEBUG_ROM_HALTED(zero)     // 0x100
   sw   s0, DEBUG_ROM_GOING(zero)      // 0x104
   ```

   Spike reserves `DEBUG_START 0x0` to `DEBUG_END 0xfff`, and QEMU virt
   has `VIRT_DEBUG` at 0x0. The SiFive FU540 resets to about 0x1004 with
   its debug module at 0. Rocket puts its boot ROM at 0x10000.
2. **Null pointers fault.** Without an MMU there's no page table to unmap
   page 0, so the only protection is that nothing useful lives there.
3. **Reset is separate from the trap vector.** Unlike ARM, where reset is
   entry 0 of the vector table, RISC-V traps go wherever `mtvec` points.
   The reset address is just a hard-wired constant. x86 resets near the
   top of memory, at 0xFFFFFFF0.

For your core, the reset PC is an RTL parameter. Putting it at 0 works if
you'll never add a standard debug module. If you might, which helps a lot
in bring-up, keep 0x0-0xfff for it and put the ROM at 0x1000 or 0x10000.

## 6. What the DTB describes (and what it doesn't)

A DTB describes hardware for the kernel to use after boot. It doesn't
describe the reset vector, the boot ROM or the debug module, and neither
the QEMU nor the Spike DTB has nodes for them. By the time anything reads
the DTB, reset is over, and Linux has no driver for a boot ROM or debug
module.

Generated SoC trees such as Chipyard/Rocket still include nodes for them,
for documentation and tools. They look like this:

```dts
debug-controller@0 {
    compatible = "sifive,debug-013", "riscv,debug-013";
    reg = <0x0 0x1000>;
    reg-names = "control";
    interrupts-extended = <&cpu0_intc 65535>;
};
rom@10000 {
    compatible = "sifive,rom0";
    reg = <0x10000 0x10000>;
    reg-names = "mem";
};
```

Linux 6.18 has no bindings or drivers for these compatibles, so it skips
them.

Linux uses whatever is in `memory` as RAM, so the only thing you must
describe is memory inside that range that the kernel must not use:

```dts
/dts-v1/;
/memreserve/ 0x87f00000 0x100000;        /* kernel won't allocate this */

/ {
    reserved-memory {
        #address-cells = <1>; #size-cells = <1>; ranges;
        loader_data@87f00000 { reg = <0x87f00000 0x100000>; no-map; };
    };
    /* ... */
};
```

Either form works. `no-map` matters on MMU kernels and does no harm here.

## 7. Seeing the DTBs yourself

```sh
export PATH=$PWD/../rv32_build/host/bin:$PATH      # from custom_kernel_ext/

# QEMU: write the DTB and exit
qemu-system-riscv32 -M virt,dumpdtb=virt.dtb -cpu rv32,mmu=off -m 128M
dtc -I dtb -O dts virt.dtb

# Spike: print the DTS and exit
spike --isa=rv32imafdc_zicntr_zihpm -m128 --dump-dts ../rv32_build/images/kernel.elf

# From inside a running guest
ls /proc/device-tree/          # the unflattened tree
cat /sys/firmware/fdt > /tmp/x.dtb   # the raw blob the kernel was given
```

### Spike DTS (`--isa=rv32imafdc_zicntr_zihpm -m128`)

```dts
/dts-v1/;

/ {
  #address-cells = <2>;
  #size-cells = <2>;
  compatible = "ucbbar,spike-bare-dev";
  model = "ucbbar,spike-bare";
  chosen {
    stdout-path = &SERIAL0;
    bootargs = "console=ttyS0 earlycon";
  };
  cpus {
    #address-cells = <1>;
    #size-cells = <0>;
    timebase-frequency = <10000000>;
    CPU0: cpu@0 {
      device_type = "cpu";
      reg = <0>;
      status = "okay";
      compatible = "riscv";
      riscv,isa = "rv32imafdc_zicntr_zihpm";
      mmu-type = "riscv,sv32";
      riscv,pmpregions = <16>;
      riscv,pmpgranularity = <4>;
      clock-frequency = <1000000000>;
      CPU0_intc: interrupt-controller {
        #address-cells = <2>;
        #interrupt-cells = <1>;
        interrupt-controller;
        compatible = "riscv,cpu-intc";
      };
    };
  };
  memory@80000000 {
    device_type = "memory";
    reg = <0x0 0x80000000 0x0 0x8000000>;
  };
  soc {
    #address-cells = <2>;
    #size-cells = <2>;
    compatible = "ucbbar,spike-bare-soc", "simple-bus";
    ranges;
    clint@2000000 {
      compatible = "riscv,clint0";
      interrupts-extended = <&CPU0_intc 3 &CPU0_intc 7 >;
      reg = <0x0 0x2000000 0x0 0xc0000>;
    };
    PLIC: plic@c000000 {
      compatible = "riscv,plic0";
      #address-cells = <2>;
      interrupts-extended = <&CPU0_intc 11 &CPU0_intc 9 >;
      reg = <0x0 0xc000000 0x0 0x1000000>;
      riscv,ndev = <0x1f>;
      riscv,max-priority = <0xf>;
      #interrupt-cells = <1>;
      interrupt-controller;
    };
    SERIAL0: ns16550@10000000 {
      compatible = "ns16550a";
      clock-frequency = <10000000>;
      interrupt-parent = <&PLIC>;
      interrupts = <1>;
      reg = <0x0 0x10000000 0x0 0x100>;
      reg-shift = <0x0>;
      reg-io-width = <0x1>;
    };
  };
  htif {
    compatible = "ucb,htif0";
  };
};
```

`mmu-type` says Spike's core has an MMU. This kernel doesn't use it.
`htif` is Spike's host interface. The PLIC also lists an S-mode context
(9), which the kernel disables.

### QEMU virt DTS (`-cpu rv32,mmu=off -m 128M`, decompiled)

<details>
<summary>Full DTS (click to expand)</summary>

```dts
/dts-v1/;

/ {
	#address-cells = <0x02>;
	#size-cells = <0x02>;
	compatible = "riscv-virtio";
	model = "riscv-virtio,qemu";

	poweroff {
		value = <0x5555>;
		offset = <0x00>;
		regmap = <0x04>;
		compatible = "syscon-poweroff";
	};

	reboot {
		value = <0x7777>;
		offset = <0x00>;
		regmap = <0x04>;
		compatible = "syscon-reboot";
	};

	platform-bus@4000000 {
		interrupt-parent = <0x03>;
		ranges = <0x00 0x00 0x4000000 0x2000000>;
		#address-cells = <0x01>;
		#size-cells = <0x01>;
		compatible = "qemu,platform", "simple-bus";
	};

	memory@80000000 {
		device_type = "memory";
		reg = <0x00 0x80000000 0x00 0x8000000>;
	};

	cpus {
		#address-cells = <0x01>;
		#size-cells = <0x00>;
		timebase-frequency = <0x989680>;

		cpu@0 {
			phandle = <0x01>;
			device_type = "cpu";
			reg = <0x00>;
			status = "okay";
			compatible = "riscv";
			riscv,cbop-block-size = <0x40>;
			riscv,cboz-block-size = <0x40>;
			riscv,cbom-block-size = <0x40>;
			riscv,isa-extensions = "i", "m", "a", "f", "d", "c", "h", "zic64b", "zicbom", "zicbop", "zicboz", "ziccamoa", "ziccif", "zicclsm", "ziccrse", "zicntr", "zicsr", "zifencei", "zihintntl", "zihintpause", "zihpm", "zmmul", "za64rs", "zaamo", "zalrsc", "zawrs", "zfa", "zca", "zcf", "zcd", "zba", "zbb", "zbc", "zbs", "sdtrig", "shcounterenw", "shgatpa", "shtvala", "shvsatpa", "shvstvala", "shvstvecd", "ssccptr", "sscounterenw", "ssstrict", "sstc", "sstvala", "sstvecd", "ssu64xl", "svadu", "svvptc";
			riscv,isa-base = "rv32i";
			riscv,isa = "rv32imafdch_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_zicntr_zicsr_zifencei_zihintntl_zihintpause_zihpm_zmmul_za64rs_zaamo_zalrsc_zawrs_zfa_zca_zcf_zcd_zba_zbb_zbc_zbs_sdtrig_shcounterenw_shgatpa_shtvala_shvsatpa_shvstvala_shvstvecd_ssccptr_sscounterenw_ssstrict_sstc_sstvala_sstvecd_ssu64xl_svadu_svvptc";
			mmu-type = "riscv,sv32";

			interrupt-controller {
				#interrupt-cells = <0x01>;
				interrupt-controller;
				compatible = "riscv,cpu-intc";
				phandle = <0x02>;
			};
		};

		cpu-map {

			cluster0 {

				core0 {
					cpu = <0x01>;
				};
			};
		};
	};

	pmu {
		riscv,event-to-mhpmcounters = <0x01 0x01 0x7fff9 0x02 0x02 0x7fffc 0x10019 0x10019 0x7fff8 0x1001b 0x1001b 0x7fff8 0x10021 0x10021 0x7fff8>;
		compatible = "riscv,pmu";
	};

	fw-cfg@10100000 {
		dma-coherent;
		reg = <0x00 0x10100000 0x00 0x18>;
		compatible = "qemu,fw-cfg-mmio";
	};

	flash@20000000 {
		bank-width = <0x04>;
		reg = <0x00 0x20000000 0x00 0x2000000 0x00 0x22000000 0x00 0x2000000>;
		compatible = "cfi-flash";
	};

	aliases {
		serial0 = "/soc/serial@10000000";
	};

	chosen {
		stdout-path = "/soc/serial@10000000";
		rng-seed = <0xd7dbabe8 0xbad38d16 0x3fd8d999 0x46df1a0e 0xf46893d8 0x860c9fb1 0x3e17fe9a 0xff13b417>;
	};

	soc {
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		compatible = "simple-bus";
		ranges;

		rtc@101000 {
			interrupts = <0x0b>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x101000 0x00 0x1000>;
			compatible = "google,goldfish-rtc";
		};

		serial@10000000 {
			interrupts = <0x0a>;
			interrupt-parent = <0x03>;
			clock-frequency = "", "8@";
			reg = <0x00 0x10000000 0x00 0x100>;
			compatible = "ns16550a";
		};

		test@100000 {
			phandle = <0x04>;
			reg = <0x00 0x100000 0x00 0x1000>;
			compatible = "sifive,test1", "sifive,test0", "syscon";
		};

		virtio_mmio@10008000 {
			interrupts = <0x08>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10008000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10007000 {
			interrupts = <0x07>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10007000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10006000 {
			interrupts = <0x06>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10006000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10005000 {
			interrupts = <0x05>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10005000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10004000 {
			interrupts = <0x04>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10004000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10003000 {
			interrupts = <0x03>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10003000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10002000 {
			interrupts = <0x02>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10002000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		virtio_mmio@10001000 {
			interrupts = <0x01>;
			interrupt-parent = <0x03>;
			reg = <0x00 0x10001000 0x00 0x1000>;
			compatible = "virtio,mmio";
		};

		plic@c000000 {
			phandle = <0x03>;
			riscv,ndev = <0x5f>;
			reg = <0x00 0xc000000 0x00 0x600000>;
			interrupts-extended = <0x02 0x0b 0x02 0x09>;
			interrupt-controller;
			compatible = "sifive,plic-1.0.0", "riscv,plic0";
			#address-cells = <0x00>;
			#interrupt-cells = <0x01>;
		};

		clint@2000000 {
			interrupts-extended = <0x02 0x03 0x02 0x07>;
			reg = <0x00 0x2000000 0x00 0x10000>;
			compatible = "sifive,clint0", "riscv,clint0";
		};

		pci@30000000 {
			interrupt-map-mask = <0x1800 0x00 0x00 0x07>;
			interrupt-map = <0x00 0x00 0x00 0x01 0x03 0x20 0x00 0x00 0x00 0x02 0x03 0x21 0x00 0x00 0x00 0x03 0x03 0x22 0x00 0x00 0x00 0x04 0x03 0x23 0x800 0x00 0x00 0x01 0x03 0x21 0x800 0x00 0x00 0x02 0x03 0x22 0x800 0x00 0x00 0x03 0x03 0x23 0x800 0x00 0x00 0x04 0x03 0x20 0x1000 0x00 0x00 0x01 0x03 0x22 0x1000 0x00 0x00 0x02 0x03 0x23 0x1000 0x00 0x00 0x03 0x03 0x20 0x1000 0x00 0x00 0x04 0x03 0x21 0x1800 0x00 0x00 0x01 0x03 0x23 0x1800 0x00 0x00 0x02 0x03 0x20 0x1800 0x00 0x00 0x03 0x03 0x21 0x1800 0x00 0x00 0x04 0x03 0x22>;
			ranges = <0x1000000 0x00 0x00 0x00 0x3000000 0x00 0x10000 0x2000000 0x00 0x40000000 0x00 0x40000000 0x00 0x40000000 0x3000000 0x03 0x00 0x03 0x00 0x01 0x00>;
			reg = <0x00 0x30000000 0x00 0x10000000>;
			dma-coherent;
			bus-range = <0x00 0xff>;
			linux,pci-domain = <0x00>;
			device_type = "pci";
			compatible = "pci-host-ecam-generic";
			#size-cells = <0x02>;
			#interrupt-cells = <0x01>;
			#address-cells = <0x03>;
		};
	};
};
```

</details>

Notes on the QEMU tree:
- **Phandles are numbers.** Decompiling a blob loses the labels:
  `0x02` is the CPU's interrupt controller, `0x03` the PLIC and `0x04`
  the `test` syscon used by poweroff/reboot.
- **`clock-frequency = "", "8@"` is a dtc misreading.** It's the 32-bit
  value 0x00384000 = 3686400 Hz, and dtc decoded it as strings.
- **The ISA string is QEMU's default `rv32` CPU, including `h`.** The
  kernel only uses what it supports and needs `rv32ima` at minimum.
- **The `rng-seed` changes every boot.**
