# custom_rv32

A Buildroot external tree (`BR2_EXTERNAL`) that builds a no-MMU, M-mode
RV32IMAFDC Linux 6.18.7 kernel with a busybox initramfs. The same
`kernel.elf` boots on QEMU `virt`, on Spike and on a custom core. Build and
run with the steps below. For how the image works, the device tree and
custom-core requirements, see
[board/custom_rv32/readme.txt](board/custom_rv32/readme.txt).

To boot the kernel without building anything, use the prebuilt image:
see [Run without building](#run-without-building-prebuilt).

## Run without building (prebuilt/)

`prebuilt/` has a ready-to-boot copy of the image from a known build. It
only needs your system's QEMU or Spike:

- QEMU: Debian/Ubuntu `sudo apt install qemu-system-misc`, Arch
  `sudo pacman -S qemu-system-riscv`. Tested with QEMU 11.0 and 11.1.
- Spike: most distros don't package it. Build it from
  [riscv-isa-sim](https://github.com/riscv-software-src/riscv-isa-sim).
  It also needs `dtc` (`device-tree-compiler` on Debian/Ubuntu, `dtc` on
  Arch) at run time.

```sh
cd custom_kernel_ext
prebuilt/start-qemu.sh      # quit: Ctrl-a x
prebuilt/start-spike.sh     # quit: Ctrl-c Ctrl-c
QEMU=/path/to/qemu-system-riscv32 prebuilt/start-qemu.sh   # another binary (SPIKE= for Spike)
```

Log in as `root` (no password).

| File                    | What it is                                                     |
|-------------------------|----------------------------------------------------------------|
| `kernel.elf`            | Kernel + initramfs, the same file as `images/kernel.elf`       |
| `start-qemu.sh`, `start-spike.sh` | The same emulator options as `images/`, but using the emulators in your `PATH` |
| `config/linux.config`, `config/busybox.config` | The full kernel and busybox configs it was built with (the tree itself only has the fragments) |
| `BUILD-INFO.txt`        | Git version, build date, component versions, sha256 of each file |

What doesn't work with system emulators:

- **Tracing.** `trace-qemu.sh` loads the `tracewin` plugin, which has to
  be built against the same QEMU version: the plugin API changed between
  11.0 and 11.1, so it doesn't even compile against 11.1's header.
  `trace-spike.sh` needs this tree's two Spike patches (trace window,
  cache statistics). A stock Spike quietly traces everything from reset.
  For tracing, build the tree.
- **Changing the image.** Programs, configs and the kernel all need the
  build.

To update `prebuilt/` from your build (after `make -C ../rv32_build`):

```sh
board/custom_rv32/update-prebuilt.sh        # reads ../rv32_build by default
git add prebuilt && git commit
```

Builds never touch `prebuilt/` themselves. It changes only when you run
the script, so it always matches its `BUILD-INFO.txt`. `kernel.elf` is
4 MB, and git keeps every version committed, so update it when a version
is worth sharing rather than after every build.

## Requirements

- Linux host with about 20 GB free: the build directory is about 16 GB
  (mostly the QEMU build) and downloads are about 1.3 GB.
- The tools Buildroot needs
  ([manual, "System requirements"](https://buildroot.org/downloads/manual/manual.html#requirement)):
  `make gcc g++ binutils bash patch gzip bzip2 perl tar cpio unzip rsync
  file bc findutils gawk wget sed which diffutils git`, plus
  `python3`.

  - Debian/Ubuntu: `sudo apt install build-essential git wget cpio unzip rsync bc file python3`
  - Arch: `sudo pacman -S --needed base-devel git wget cpio unzip rsync bc python`

Buildroot builds everything else from source into the output directory:
the riscv32 cross toolchain (uClibc-ng), `dtc`, `qemu-system-riscv32` and
`spike`. You don't need a system QEMU or Spike.

## Check out

The build is out of tree. Put three directories side by side:

```
<work>/
    custom_kernel/        Buildroot
    custom_kernel_ext/    this repo
    rv32_build/           build output (created by the configure step)
```

```sh
mkdir rv32 && cd rv32

git clone <this-repo-url> custom_kernel_ext

# Buildroot, pinned to the commit this tree was built and tested with
git clone https://github.com/buildroot/buildroot.git custom_kernel
git -C custom_kernel checkout 89d584b8e2c8cda252c768dafac020f79d739691
```

Buildroot `master` keeps moving, so use the pinned commit unless you
plan to retest on a newer one. If you change it, update the hash here.

## Build

Run these from `custom_kernel_ext/`:

```sh
cd custom_kernel_ext

# Configure: creates ../rv32_build from configs/custom_rv32_defconfig
make -C ../custom_kernel O=$PWD/../rv32_build \
     BR2_EXTERNAL=$PWD custom_rv32_defconfig

# Build everything (toolchain, qemu, spike, busybox, kernel)
make -C ../rv32_build
```

Don't add `-j`. Buildroot builds one package at a time and runs each
package's own build in parallel. The first build takes a while. Later
builds only redo what changed. Compiler output is cached in
`~/.buildroot-ccache` (`BR2_CCACHE`).

The results are in `../rv32_build/images/`:

| File             | What it is                                                   |
|------------------|--------------------------------------------------------------|
| `kernel.elf`     | Kernel + initramfs, load address = 0x80000000. Boot this.    |
| `vmlinux`        | Same kernel with 0-based load addresses (use for gdb symbols)|
| `rootfs.cpio`    | The initramfs (already linked into the kernel)               |
| `start-qemu.sh`  | Boots `kernel.elf` on the QEMU built above                   |
| `start-spike.sh` | Boots `kernel.elf` on the Spike built above                  |
| `trace-qemu.sh`, `trace-spike.sh` | Boot with tracing, see [Tracing](#tracing)  |

## Run

```sh
../rv32_build/images/start-qemu.sh      # quit: Ctrl-a x
../rv32_build/images/start-spike.sh     # quit: Ctrl-c Ctrl-c (one Ctrl-c opens Spike's debug prompt)
```

Log in as `root` (no password). Arguments go straight to the emulator,
e.g. `start-qemu.sh -s -S` waits for gdb on port 1234. Set `QEMU=` or
`SPIKE=` to use another emulator binary.

## Tracing

`trace-qemu.sh` and `trace-spike.sh` (also in `images/`) record what
runs between two trace markers. Boot runs at nearly full speed, and only
your window is logged:

```sh
../rv32_build/images/trace-spike.sh -o ls.trace -u -d -x "ls /"   # Spike commit log, user mode only
../rv32_build/images/trace-qemu.sh  -o ls.trace -x "ls /"         # QEMU: insns + loads/stores
../rv32_build/images/trace-spike.sh -N -I 128:4:64 -D 128:4:64 -x "ls /"  # Spike: I$/D$ miss rates only
```

`-x` runs a command at boot between the markers and exits. Without it,
run `tracemark on` / `tracemark off` in the shell, or put
`TRACE_ON()`/`TRACE_OFF()` from `<trace_marker.h>` in your code.
`../rv32_build/images/trace-stats.py --split ls.trace` prints the
instruction mix (branches and % taken, loads/stores, compressed share,
basic block length, footprint). `../rv32_build/images/trace-workloads.sh`
captures a standard set of workloads (`ls`, `memhog`, `lua`, `coremark`) and saves
compressed traces plus their stats in `rv32_build/traces/`. See
[docs/tracing.md](docs/tracing.md).

## Adding programs

See [docs/adding-programs.md](docs/adding-programs.md): a C program with a
Makefile, a CMake project, one-off binaries, scripts and data files, and
what to rebuild after each kind of change. In short:

- Standard tools (`free`, `top`, `vi`, ...) are busybox applets: add
  `CONFIG_<APPLET>=y` to `board/custom_rv32/busybox.fragment` and run
  `make -C ../rv32_build`.

- Put the source in `src/<name>/` and a package recipe in `package/<name>/`
  (copy `package/memhog/` + `src/memhog/`). Add a `source` line to
  `Config.in` and `BR2_PACKAGE_<NAME>=y` to the defconfig.
- Binaries must be FLAT. Buildroot's `CFLAGS`/`LDFLAGS` do that, so use
  them, with `override CFLAGS += ...` in your Makefile.
- C only, static only, fixed stack (`<NAME>_FLAT_STACKSIZE`; anything left
  at elf2flt's 4 KiB default is raised to 32 KiB by `post-build.sh`), no
  `fork()`.
- No compiler runs on the target. `lua`/`luac`, `coremark`, `dhrystone` and
  `whetstone` are included as workloads.
- After editing source, run `make -C ../rv32_build <name>-rebuild all`.

## Changing things

| To change...             | Edit                                     | Then run                                        |
|--------------------------|------------------------------------------|-------------------------------------------------|
| Kernel options           | `board/custom_rv32/linux-nommu.fragment` | `make -C ../rv32_build`                         |
| Files in the rootfs      | `board/custom_rv32/overlay/`             | `make -C ../rv32_build`                         |
| Buildroot options        | `make -C ../rv32_build menuconfig`       | `make -C ../rv32_build savedefconfig` and build |
| Kernel source (hack)     | `../rv32_build/build/linux-6.18.7/`      | `make -C ../rv32_build linux-rebuild`           |

`board/custom_rv32/readme.txt` lists more targets and explains how to add a device tree.

[docs/boot-and-device-tree.md](docs/boot-and-device-tree.md) explains how the
kernel is started and gets its device tree, on the emulators and on real hardware.
