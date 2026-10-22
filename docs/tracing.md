# Capturing traces

`rv32_build/images/` has two trace scripts next to the start scripts:

| Script           | Emulator | What it records                                                        |
|------------------|----------|------------------------------------------------------------------------|
| `trace-qemu.sh`  | QEMU     | PC, instruction, disassembly, loads/stores with values, optional register writes (`tracewin` plugin) |
| `trace-spike.sh` | Spike    | Spike's commit log: privilege, PC, instruction, register writes, memory accesses (optional disassembly), and/or cache statistics from Spike's I$/D$/L2 model |

Both record only what runs between two **trace markers**, so you can boot
Linux at nearly full speed and trace just your program. All examples were
run with this tree. Paths are relative to `custom_kernel_ext/`.

## Quick start

Trace one command, non-interactively. The image boots, runs the command
between the markers, and the emulator exits:

```sh
../rv32_build/images/trace-qemu.sh  -o ls.qemu.trace  -x "ls /"
../rv32_build/images/trace-spike.sh -o ls.spike.trace -x "ls /" -u -d
```

Expect big files even for small commands. `ls /` took 3 s and 2.27M
instructions (about 160 MB) on QEMU, and 4 s and 2.31M instructions
(130 MB) on Spike. Spike's breakdown: 92% of the window is the kernel, mostly
`memcpy` and `load_flat_binary`. Without an MMU, every `exec` copies the
whole program into RAM and patches its relocations. Only 192K
instructions were `ls` itself, and that's what `-u` keeps. For a tight
window, put the markers in your code (below).

Or interactively. Boot, log in as `root`, then:

```sh
../rv32_build/images/trace-qemu.sh -o my.trace
~ # tracemark on; my-program; tracemark off
~ # poweroff -f                       # Spike: press Ctrl-c twice instead
```

The trace is written when the emulator exits.

## Trace markers

The window starts at one instruction and ends at another:

| Marker    | Instruction          | Encoding     |
|-----------|----------------------|--------------|
| trace on  | `addi x0, x0, 0x5a1` | `0x5a100013` |
| trace off | `addi x0, x0, 0x5a0` | `0x5a000013` |

Both are RISC-V **HINT** instructions: they write `x0`, so they're no-ops
on every core. You can leave them in code that also runs in RTL simulation
or on your hardware, and use the same encodings to trigger trace capture
in your testbench.

Three ways to execute them:

- **From the shell:** `tracemark on` / `tracemark off` (package
  `tracemark`). The window then includes the shell starting your program
  and everything the kernel does in between.
- **In your C code**, for a tight window around the part you care about:

  ```c
  #include <trace_marker.h>

  TRACE_ON();
  hot_loop();
  TRACE_OFF();
  ```

  `trace_marker.h` is installed into the Buildroot sysroot. Add
  `tracemark` to your package's `_DEPENDENCIES` so it exists before your
  package builds. It's a short header (`src/tracemark/trace_marker.h`),
  so you can also copy it.
- **`-x CMD`:** the boot script `/etc/init.d/S99autorun` runs
  `tracemark on; CMD; tracemark off` and prints `@AUTORUN_DONE@`. The
  trace scripts watch for that line and stop the emulator. CMD is passed
  on the kernel command line as `autorun="CMD"`, so it can't contain
  double quotes. Its output appears on the console as usual.

The trace is **whole-system**: while the window is open, every
instruction is recorded, including other processes, interrupts and the
kernel. With Spike, `-u` keeps only user-mode instructions.

## trace-qemu.sh (QEMU + tracewin)

```
trace-qemu.sh [options] [-- extra QEMU args]
  -o FILE   trace file (default: qemu.trace)
  -x CMD    run CMD in the guest at boot, traced, then exit
  -a        trace everything from reset, ignoring the markers
  -r        also log integer register writes
  -M        don't log loads/stores
  -n N      stop tracing after N instructions
  -p NAME   use QEMU plugin libNAME.so instead (see below)
  -R        guest time follows the host clock (see "Guest time" below)
```

Output, one line per instruction:

```
0x8072c6d4 (0x5a100013) addi                    zero,zero,1441
0x8072c6d8 (0x4462) lw                      s0,24(sp) load 0x8073bee8 0x8073bf24
0x8072c6da (0x4501) mv                      a0,zero
```

- **Fields:** PC, the instruction bits (4 hex digits for compressed
  instructions), the disassembly, then ` load|store ADDRESS VALUE` for
  each memory access. With `-r`, each changed integer register is
  appended as ` a0=0x00000009`.
- **No privilege field.** QEMU's plugin API doesn't report the privilege
  level, so the trace can't separate user from kernel code. Use Spike
  with `-u` for that.
- **Addresses are physical.** There's no MMU, so they match what your
  core would see.

### Guest time: why QEMU needs -icount

By default `trace-qemu.sh` runs QEMU with `-icount shift=0,sleep=off`:
guest time advances 1 ns per instruction, the same as Spike (1 GHz CPU,
10 MHz `mtime`). Without it (`-R`), QEMU's `mtime` follows the host
clock. Inside a window QEMU runs about 1-2M instructions per second
instead of 100M+, so the guest takes far more timer interrupts per
instruction, and the trace fills with kernel
timekeeping and scheduler code. Measured on `ls /` (exact counts shift
a little whenever the image changes, but each build gives the same
count on every run):

| Run                         | Instructions | Timer interrupts | Repeatable? |
|-----------------------------|--------------|------------------|-------------|
| Spike                       | 2,314,573    | 0                | yes         |
| QEMU (default, icount)      | 2,273,751    | 0                | yes, same count every run |
| QEMU `-R`                   | 4.3M-9.1M    | 439-1088         | no: slower tracing (e.g. memory logging) means more ticks |

With `-R`, each tick cost about 4,600 instructions:
`__timekeeping_advance`, scheduler load tracking (`decay_load`,
`__update_load_avg_*`), and the 64-bit division and shift helpers that
math needs on rv32 (`__div64_32`, `__lshrdi3`). The user-mode part
(192K instructions) was identical.

The remaining 1.8% between QEMU and Spike comes from their different
hardware, which the kernel adapts to at boot:

- **Zbb.** QEMU's default `rv32` CPU has it, so byte swaps use `rev8`.
  The FLAT loader byte-swaps every relocation: `__fswab32` is 12K
  instructions on QEMU and 26K on Spike, whose ISA string has no Zbb.
- **Zicboz.** QEMU has it, so `clear_page` zeroes 64 bytes per
  instruction instead of calling `memset`.
- **RNG seed.** QEMU's device tree passes an `rng-seed`, so the kernel's
  random number generator is ready at boot. Spike's has none, so the
  kernel is still mixing entropy (`blake2s_compress_generic`, +35K on
  Spike).

To compare the two exactly, make both describe the same core: the same
ISA (e.g. QEMU `-cpu rv32,mmu=off,zbb=false,zicboz=false,...` or Spike
`--isa=...`), and a DTB of your own (`-dtb` / `--dtb`). That is also what
your RTL comparison will need.

With `sleep=off`, the guest clock jumps ahead while the guest is idle,
for example while it waits at the shell prompt (`uptime` can show
minutes after a few seconds). That doesn't affect the trace.

### Speed (boot to login prompt on QEMU)

| Setup                                   | Time   |
|-----------------------------------------|--------|
| No plugin                               | 0.8 s  |
| `tracewin`, waiting for the marker (default `-icount`) | 1.9 s  |
| `tracewin` with `-M` (no memory)        | 1.3 s  |
| QEMU's stock `execlog` (logs everything)| 76 s   |

Inside the window, expect 1-2M instructions per second and **about 72
bytes per instruction**: 1M instructions is about 70 MB. Use `-n` to cap
a run, and `-M` to drop memory accesses.

### Other QEMU plugins (`-p`)

QEMU's own plugins are built too (`rv32_build/host/lib/qemu-plugins/`).
They cover **the whole run** and ignore the markers. Options go after
commas:

```sh
trace-qemu.sh -p bb -x true                     # instruction and block counts
trace-qemu.sh -p cache -x coremark              # L1 I/D cache model: miss rates, top missing insns
trace-qemu.sh -p cache,dcachesize=16384,dassoc=4 -x coremark
trace-qemu.sh -p hotblocks -x true              # most executed translation blocks
```

```
core #, data accesses, data misses, dmiss rate, insn accesses, insn misses, imiss rate
0       350504538      1605821         0.4581%  1353812133     1781359         0.1316%
```

See `rv32_build/build/host-qemu-11.0.0/contrib/plugins/*.c` for each
plugin's options. QEMU's `libinsn.so` test plugin crashes on this CPU
(it asserts while reading a register). Use `-p bb` for instruction
counts.

## trace-spike.sh (Spike commit log)

```
trace-spike.sh [options] [-- extra Spike args]
  -o FILE   trace file (default: spike.trace)
  -x CMD    run CMD in the guest at boot, traced, then exit
  -a        trace everything from reset, ignoring the markers
  -u        keep only user-mode (priv 0) instructions
  -d        add disassembly to each line (spike-dasm)
  -I S:W:B  model an L1 I-cache: S sets, W ways, B-byte lines (--ic)
  -D S:W:B  model an L1 D-cache (--dc)
  -L S:W:B  model an L2 behind both L1s (--l2)
  -c FILE   cache statistics file (default: spike.cache.txt)
  -N        no commit log, only cache statistics (much faster)
```

Output (`-d`), one line per instruction:

```
core   0: 0 0x808ac6d4 (0x5a100013) li      zero, 1441
core   0: 0 0x808ac6d8 (0x4462) c.lwsp  s0, 24(sp) x8  0x808bbf24 mem 0x808bbee8
core   0: 0 0x808ac6da (0x4501) c.li    a0, 0 x10 0x00000000
```

- **Fields:** core, privilege (0 = user, 3 = machine), PC, instruction
  bits, disassembly (with `-d`), then each register write (`x8
  0x808bbf24`) and memory access (`mem ADDRESS` for a load,
  `mem ADDRESS VALUE` for a store).
- **The usual reference for RTL co-simulation.** This is Spike's
  standard `--log-commits` format, so you can diff it against your
  core's retirement log.
- **Speed:** with the markers, Spike boots to the login prompt in 2.0 s
  (0.7 s with no logging). Inside the window, it logs about 0.9M
  instructions per second at about 50 bytes per instruction.

### How the window works on Spike

Stock Spike can't switch its commit log on and off. This tree patches it
(`board/custom_rv32/patches/riscv-isa-sim/0001-commit-log-trace-marker-gating.patch`,
applied through `BR2_GLOBAL_PATCH_DIR`). With `SPIKE_TRACE_GATE` set in
the environment, `--log-commits` only prints between the markers, and
flushes the file at the off marker. `trace-spike.sh` sets the variable
unless you pass `-a`. Without it, Spike behaves as upstream.

You can use the patched Spike directly too:

```sh
SPIKE_TRACE_GATE=1 ../rv32_build/host/bin/spike --isa=rv32imafdc_zicntr_zihpm \
    -m128 --log-commits --log=my.trace ../rv32_build/images/kernel.elf
```

### Cache statistics (-I, -D, -L)

Spike has a simple cache model: an L1 I-cache, an L1 D-cache, and an
optional unified L2 behind them. Each is `S:W:B`, meaning S sets, W
ways and B-byte lines (S and B powers of two, B at least 8). Capacity is
S × W × B, so 32 KiB 4-way with 64-byte lines is `128:4:64`.
Replacement is random (an LFSR), the D-cache is write-back and
write-allocate, and dirty L1 lines are written back to the L2.

```sh
# Cache statistics only, no trace file
../rv32_build/images/trace-spike.sh -N -I 128:4:64 -D 128:4:64 -x "ls /"
# With an L2, plus the commit log, statistics saved to ls.cache.txt
../rv32_build/images/trace-spike.sh -o ls.trace -c ls.cache.txt \
    -I 128:4:64 -D 128:4:64 -L 1024:8:64 -x "ls /"
```

The statistics are printed at the end and saved to `-c FILE`. From the second command:

```
Window Instructions:      2320632
I$ Read Accesses:         2324997
I$ Read Misses:           12825
I$ Miss Rate:             0.552%
I$ Misses per 1K Insns:   5.527
D$ Read Accesses:         574330
D$ Write Accesses:        894160
D$ Read Misses:           29650
D$ Write Misses:          47030
D$ Writebacks:            49532
D$ Miss Rate:             5.222%
D$ Misses per 1K Insns:   33.043
L2$ Read Accesses:         89505
L2$ Miss Rate:             49.333%
...
```

(Shortened. Each cache also reports bytes read and written.)

- **Same window as the trace.** Only accesses between the markers count,
  and the instructions retired in the window give misses per 1K
  instructions. Outside the window the caches are still simulated, so
  the window starts with warm caches. With `-a`, everything from reset
  counts and there is no instruction count.
- **Whole-system,** like the trace: kernel, interrupts and other
  processes count too. There's no user-only option.
- **Physical addresses,** one access per fetch, load or store. An access
  that crosses a line boundary only counts the first line. The I-cache
  counts a few more accesses than the window has instructions, because
  instructions that trap are fetched but never retire.
- **Repeatable.** The same command gives the same numbers on every run,
  with or without the commit log.
- **Speed:** `-N` on `ls /` takes 11 s, the commit log as well takes 16 s.
  With a cache attached, Spike sends every fetch through the model from
  reset, so booting is slower too. `coremark` (21G instructions) is still
  too long for Spike.

QEMU's `cache` plugin (`trace-qemu.sh -p cache`) is a faster
alternative, but it covers the whole run and ignores the markers.

You can also use the patched Spike directly. `SPIKE_CACHE_STATS` names
the statistics file (stdout if unset):

```sh
SPIKE_TRACE_GATE=1 SPIKE_CACHE_STATS=my.cache.txt ../rv32_build/host/bin/spike \
    --isa=rv32imafdc_zicntr_zihpm -m128 --ic=128:4:64 --dc=128:4:64 \
    ../rv32_build/images/kernel.elf
```

Stock Spike only prints these statistics when it exits normally. Its
`quit` command and a second Ctrl-c, which `trace-spike.sh` relies on,
lose them. The second patch,
`board/custom_rv32/patches/riscv-isa-sim/0002-cache-model-stats-at-exit-and-trace-marker-gating.patch`,
prints them on any exit, writes them to `SPIKE_CACHE_STATS`, and makes
the cache model follow the markers. It detects the markers in `addi`
itself, so it doesn't need `--log-commits`.

## Statistics: trace-stats.py

`rv32_build/images/trace-stats.py` reports the instruction mix of any
trace from either script:

```sh
../rv32_build/images/trace-stats.py ls.trace            # whole window
../rv32_build/images/trace-stats.py --split ls.trace    # plus user and kernel separately
../rv32_build/images/trace-stats.py --top 20 a.trace b.trace
```

It reports, for the whole window and (with `--split`) for user and
kernel separately:

- **Instruction classes, with counts and percentages.** branch
  (conditional), jump, call, return, indirect jump, load, store, FP
  load/store, atomic, ALU, mul/div, FP, CSR/system, fence, and
  cache-block ops (`cbo.*`).
- **Conditional branches taken, overall and split forward/backward.** The
  target is decoded from each instruction and compared with the next PC,
  so this is exact. A branch followed by a trap counts as "interrupted".
- **Code size and structure:** the share of compressed (RVC)
  instructions, and the average basic block length (instructions per
  control transfer).
- **Memory traffic:** loads and stores per 100 instructions, and the code
  and data footprint in 64-byte lines.
- **The most executed instructions.**

Instructions are classified from their encoding, not the disassembly,
so QEMU and Spike traces are counted the same way. The decoder was
checked against QEMU's disassembler on all 14,700 distinct encodings in
a trace; the only disagreement was QEMU decoding `cbo.zero` as `lq`.
The script processes about 400K lines per second (20M lines in 50 s).

Two differences between the formats:

- **`--split` on QEMU traces goes by address.** QEMU traces have no
  privilege field, so instructions inside the kernel's text (`_stext` to
  `_etext` from `images/vmlinux`) count as kernel and everything else as
  user. Kernel code outside that range (the boot code at 0x80000000, init
  code) is counted as user, but it doesn't run inside a window. Spike
  traces use the real privilege level.
- **Spike leaves out instructions that trap.** Its commit log only lists
  instructions that complete, so `ecall` (79 in the `ls /` window) and
  faulting instructions are missing. QEMU logs them.

### Saved workload traces: trace-workloads.sh

`trace-workloads.sh` captures a standard set of workloads, so the
results above can be regenerated and the traces kept for later analysis:

```sh
../rv32_build/images/trace-workloads.sh              # all workloads, QEMU and Spike
../rv32_build/images/trace-workloads.sh -e qemu lua  # one workload, one emulator
../rv32_build/images/trace-workloads.sh -l           # list the workloads
../rv32_build/images/trace-workloads.sh -s           # recompute stats from saved traces only
```

A full run takes about 1.5 minutes to capture and leaves about 72 MB of
traces. The largest is coremark's 20M-instruction trace, 1.3 GB
uncompressed and 14 MB with zstd. `-s` re-reads the saved traces
without booting anything, which takes about 2 minutes. Use it after
changing `trace-stats.py`.

| Workload   | Guest command                       | Emulators              |
|------------|-------------------------------------|------------------------|
| `ls`       | `ls /`                              | QEMU, Spike            |
| `memhog`   | `memhog -c 60 -n 8`                 | QEMU, Spike            |
| `lua`      | `lua -e '...'` (a 10,000-step loop) | QEMU, Spike            |
| `coremark` | `coremark`, first 20M instructions  | QEMU only (no cap on Spike; a full run is 21G instructions) |

Everything goes to `rv32_build/traces/` (or `-d DIR`):

```
qemu-coremark.trace.zst     compressed trace (zstd)
qemu-coremark.stats.txt     trace-stats.py --split output, with the info below at the top
qemu-coremark.info.txt      command, emulator, capture date, custom_rv32 git version
qemu-coremark.console.txt   the guest's console output (e.g. coremark's score)
```

User-mode results from one run of all workloads:

| Workload | User insns | Branches | Taken | Loads | Stores | RVC | Basic block |
|----------|-----------:|---------:|------:|------:|-------:|----:|------------:|
| ls (QEMU / Spike)     | 192K  | 17.3% | 61.8% | 19.3% | 15.0% | 62% | 3.8 |
| memhog (QEMU / Spike) | 3.15M | 10.0% | 91.9% | 1.9%  | 67.6% | 81% | 5.2 |
| lua (QEMU / Spike)    | 2.24M | 12.0% | 51.3% | 17.1% | 9.3%  | 52% | 5.5 |
| coremark (QEMU)       | 19.0M | 18.1% | 55.5% | 18.8% | 5.2%  | 56% | 4.7 |

QEMU and Spike agree to the first decimal on all of these.

To analyze a saved trace again, `trace-stats.py` reads `.zst`, `.gz`
and `.xz` directly, so you don't need to decompress it:

```sh
../rv32_build/images/trace-stats.py --split ../rv32_build/traces/qemu-coremark.trace.zst
zstdcat ../rv32_build/traces/spike-ls.trace.zst | less    # read it
```

`rv32_build/` isn't in git, and the traces belong to the image that
produced them. Re-run `trace-workloads.sh` after changing the kernel,
the rootfs or the emulators. The `.stats.txt` files are small, so copy
them somewhere tracked if you want to keep results.

To add a workload, add a line to `WORKLOADS` in
`board/custom_rv32/trace-workloads.sh.in` (`name|command|trace-qemu.sh
options|yes or no for Spike`) and run `make -C ../rv32_build`.

### Results

`ls /` (`-x "ls /"`, Spike and QEMU agree within about 1%):

| | whole window | user (`ls`) | kernel |
|---|---|---|---|
| instructions | 2.31M | 192K | 2.12M |
| conditional branches | 7.4% | 17.3% | 6.5% |
| taken | 62% | 62% | 62% |
| all control flow | 11.4% | 26.5% | 10.0% |
| loads / stores | 24.6% / 38.5% | 19.4% / 15.0% | 25.1% / 40.6% |
| compressed | 37% | 62% | 35% |
| avg basic block | 8.8 | 3.8 | 10.0 |

The kernel side is dominated by `memcpy`/`memset` loading the program
(see above): 40% stores, mostly `sw`.

coremark, the first 20M instructions (`trace-qemu.sh -n 20000000 -x
coremark`, user part = 19.0M):

| | coremark |
|---|---|
| conditional branches | 18.1% (55.5% taken: forward 22.7%, backward 90.7%) |
| all control flow | 21.3% |
| loads / stores | 18.8% / 5.2% |
| ALU / mul-div | 51.6% / 3.2% |
| compressed | 55.7% |
| avg basic block | 4.7 instructions |
| footprint | 51 KiB code, 22 KiB data |

## How it's built

| Piece                       | Where                                              |
|-----------------------------|----------------------------------------------------|
| QEMU plugins enabled        | `external.mk` (`HOST_QEMU_OPTS += --enable-plugins`, plus a hook that installs them) |
| `tracewin` plugin           | `src/qemu-tracewin/tracewin.c`, package `package/qemu-tracewin` (host) |
| Spike marker gating, cache statistics | `board/custom_rv32/patches/riscv-isa-sim/` (0001: commit log, 0002: cache model) |
| `tracemark`, `trace_marker.h` | `src/tracemark/`, package `package/tracemark`    |
| Autorun at boot             | `board/custom_rv32/overlay/etc/init.d/S99autorun`  |
| Scripts                     | `board/custom_rv32/trace-{qemu,spike}.sh.in`, `trace-stats.py.in`, `trace-workloads.sh.in`, installed by `post-image.sh` |

After changing them, run these (`make -C ../rv32_build ...`):

| Changed                     | Run                                              |
|-----------------------------|--------------------------------------------------|
| `tracewin.c`                | `host-qemu-tracewin-rebuild`                     |
| The Spike patches           | `host-riscv-isa-sim-dirclean host-riscv-isa-sim` (about 7 minutes) |
| `tracemark` / the header    | `tracemark-rebuild all`                          |
| Scripts, autorun            | `all`                                            |
