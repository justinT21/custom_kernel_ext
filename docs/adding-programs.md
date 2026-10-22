# Adding programs and files to the image

How to get your own code (and other files) compiled and into `kernel.elf`.
Every recipe below was built and run on QEMU with this tree. Paths are
relative to `custom_kernel_ext/`, and commands run from there.

## How it fits together

```
custom_kernel_ext/
    src/<name>/            your source code (any build system)
    package/<name>/        tells Buildroot how to build and install it
        Config.in          the BR2_PACKAGE_<NAME> option
        <name>.mk          build/install recipe
    Config.in              sources every package/<name>/Config.in
    external.mk            includes every package/*/*.mk (already set up)
    configs/custom_rv32_defconfig   turns packages on (BR2_PACKAGE_<NAME>=y)
    board/custom_rv32/overlay/      files copied as-is into the rootfs
```

`make -C ../rv32_build` builds each enabled package and installs it into
`../rv32_build/target/`. It then packs `target/` into `rootfs.cpio` and
relinks the kernel with it, because the rootfs is an initramfs inside
`kernel.elf`. There is no separate filesystem image to update.

## Rules for this target

- **Binaries must be FLAT** (`file` says `BFLT executable`). The kernel
  has no plain ELF loader. A normal ELF fails with `applet not found`.
  Buildroot makes FLAT automatically by adding `-fPIC -Wl,-elf2flt=-r` to
  `CFLAGS`/`LDFLAGS`. Your build must use those variables.
- **C only.** The toolchain was built without C++ (`CXX=no`). For C++,
  set `BR2_TOOLCHAIN_BUILDROOT_CXX=y`, which means a full rebuild
  (`make -C ../rv32_build clean all`).
- **Static only.** There are no shared libraries (`BR2_STATIC_LIBS=y`).
  Libraries are linked into each program.
- **Fixed stack.** Without an MMU the stack can't grow. Set the size with
  `<NAME>_FLAT_STACKSIZE` (bytes, e.g. `0x4000`). Otherwise elf2flt's
  default of 4 KiB is too small for many programs: sqlite3 segfaults with
  it. `board/custom_rv32/post-build.sh` raises any binary still at 4 KiB
  to 32 KiB (`FLAT_MIN_STACK`) and prints each one it changes.
- **No `fork()`.** Use `vfork()` + `exec`, or `posix_spawn()`.
- **Every byte costs RAM per process.** Each running program takes
  text + data + bss + stack as one contiguous block. See "Minimum RAM" in
  `board/custom_rv32/readme.txt`.

## Standard tools (`free`, `top`, `find`, ...): busybox applets

Most standard commands come from busybox. For no-MMU builds, Buildroot
uses `package/busybox/busybox-minimal.config`, which leaves many of them
out. This tree adds more with
[board/custom_rv32/busybox.fragment](../board/custom_rv32/busybox.fragment),
which the defconfig loads with
`BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES`.

To add an applet, add a line and rebuild:

```sh
echo 'CONFIG_VI=y' >> board/custom_rv32/busybox.fragment
make -C ../rv32_build                   # rebuilds busybox and the kernel image
```

To find the option name, browse interactively. `busybox-diff-config`
prints what changed, so you can copy those lines into the fragment:

```sh
make -C ../rv32_build busybox-menuconfig     # explore, select applets, save
make -C ../rv32_build busybox-diff-config    # shows e.g. "+FEATURE_VI_COLON y"
```

Changes made only in `busybox-menuconfig` are lost the next time the
config is regenerated, so put the lines you want in the fragment. To
remove an applet, delete its line, rebuild, and delete its leftover
symlink from `../rv32_build/target/` (e.g. `target/usr/bin/<applet>`).

Currently added: `free top uptime watch pidof find stat less hexdump
strings devmem`. `devmem` also needs `CONFIG_DEVMEM=y` in
`linux-nommu.fragment` (already set). Tested on QEMU:

```
~ # free -k
              total        used        free      shared  buff/cache   available
Mem:         127536        3060      122624           0        1852      120476
~ # devmem 0x10000005 8          # ns16550 line status register
0x60
```

Notes:

- **Some applets don't work without an MMU.** Test every applet you add.
  `timeout` never kills the command and `time` reports garbage user time,
  so the fragment leaves both out.
- **`top` shows RSS 0 for every process.** No-MMU has no RSS accounting.
  Use the `Mem:` line in `/proc/<pid>/status`.
- **Each applet makes every busybox process bigger.** Each process holds
  its own copy of busybox in RAM. The current set added 14 KiB (busybox
  went from 592 to 604 KiB per process). Check with
  `flthdr ../rv32_build/target/bin/busybox` (BSS End + Stack Size).
  Bigger applets such as `vi`, `awk` and `diff` cost more.

Other Buildroot packages (`make -C ../rv32_build menuconfig` → Target
packages, then `savedefconfig`) work only if they support no-MMU.
Anything that needs `fork()` is hidden from the menu.

## Workloads: what else runs on the target

The defconfig includes these Buildroot packages as realistic CPU
workloads. All were tested on QEMU:

| Package     | Command                    | Notes                                                  |
|-------------|----------------------------|--------------------------------------------------------|
| `lua`       | `lua`, `luac`              | Interpreter and bytecode compiler: `luac -o t.luac t.lua && lua t.luac` |
| `coremark`  | `coremark`                 | Validates and reports Iterations/Sec (wall clock)      |
| `dhrystone` | `dhrystone N`              | Runs, but can't report a score (see below)             |
| `whetstone` | `whetstone LOOPS`          | Floating-point benchmark. Times in whole seconds of wall clock, so use enough loops to run several seconds |

`sqlite` (`sqlite3 :memory:`) also works, but isn't enabled: it adds
1.4 MiB to every process that runs it and to the rootfs, which is in RAM.

**No compiler runs on the target.** Buildroot has no on-target gcc, and
TinyCC outputs ELF (which this kernel can't run) and has no 32-bit
RISC-V backend. Programs are cross-compiled on the host (recipes below).
`luac` is the closest thing to a compiler workload in the image.

**CPU-time reporting is broken in userspace.** `times()` and
`getrusage()` return garbage in this uClibc-ng build (rv32 with 64-bit
`time_t`), even though the kernel's accounting is right:
`/proc/<pid>/stat` showed 603 ticks of user time for a 6 s coremark run.
So dhrystone reports "Measured time too small", and busybox `time` prints
nonsense. Programs that time themselves with `clock_gettime()` (like
coremark) are fine.

## Recipe 1: a C program with a Makefile (most common)

Template: `package/memhog/` + `src/memhog/`. Example with two source
files and a header:

```
src/fltdemo/
    Makefile
    main.c
    greet.c
    include/greet.h
```

`src/fltdemo/Makefile`:

```make
CFLAGS ?= -O2
override CFLAGS += -Wall -Iinclude

SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)

fltdemo: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f fltdemo fltdemo.gdb $(OBJS)

.PHONY: clean
```

> **Use `override`.** Buildroot passes `CFLAGS=...` on the make command
> line, and command-line variables silently replace any plain `CFLAGS +=`
> in the Makefile. Without `override`, `-Iinclude` is dropped and the
> build fails with `greet.h: No such file or directory`.

`SRCS := $(wildcard *.c)` means any new `.c` file you add to the directory
is compiled automatically. For sources in subdirectories, list them
explicitly or use `$(wildcard *.c lib/*.c)`.

`package/fltdemo/Config.in`:

```kconfig
config BR2_PACKAGE_FLTDEMO
	bool "fltdemo"
	help
	  One line about what it is.
```

`package/fltdemo/fltdemo.mk`:

```make
FLTDEMO_VERSION = 1.0
FLTDEMO_SITE = $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/src/fltdemo
FLTDEMO_SITE_METHOD = local
FLTDEMO_FLAT_STACKSIZE = 0x4000

define FLTDEMO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define FLTDEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fltdemo $(TARGET_DIR)/usr/bin/fltdemo
endef

$(eval $(generic-package))
```

Naming rules: the directory, the `.mk` file and the `$(eval ...)` package
are `fltdemo` (lowercase). Every variable is the uppercase name
(`FLTDEMO_...`, `BR2_PACKAGE_FLTDEMO`). A `-` in the name becomes `_` in
variables (`my-tool` → `MY_TOOL_SITE`).

Hook it up:

```sh
# 1. Make the option visible
echo 'source "$BR2_EXTERNAL_CUSTOM_RV32_PATH/package/fltdemo/Config.in"' >> Config.in

# 2. Turn it on in the saved config
echo 'BR2_PACKAGE_FLTDEMO=y' >> configs/custom_rv32_defconfig

# 3. Reload the config and build
make -C ../custom_kernel O=$PWD/../rv32_build BR2_EXTERNAL=$PWD custom_rv32_defconfig
make -C ../rv32_build
```

Then in the guest:

```
~ # fltdemo
hello, fltdemo
```

## Recipe 2: a CMake project

```
src/cmakedemo/
    CMakeLists.txt
    main.c
```

`src/cmakedemo/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.10)
project(cmakedemo C)                      # C only: no C++ toolchain
add_executable(cmakedemo main.c)          # add more sources here
install(TARGETS cmakedemo RUNTIME DESTINATION bin)
```

`package/cmakedemo/cmakedemo.mk`:

```make
CMAKEDEMO_VERSION = 1.0
CMAKEDEMO_SITE = $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/src/cmakedemo
CMAKEDEMO_SITE_METHOD = local
CMAKEDEMO_FLAT_STACKSIZE = 0x4000

$(eval $(cmake-package))
```

`Config.in` is the same as in recipe 1 (`BR2_PACKAGE_CMAKEDEMO`). With
`cmake-package` you don't need build or install commands. Buildroot runs
CMake with its toolchain file, which carries the FLAT flags, and then
runs `make install` with prefix `/usr`, so the binary lands in
`/usr/bin/`. Autotools projects work the same way with
`$(eval $(autotools-package))`.

## Recipe 3: one-off or prebuilt binaries (no package)

Compile with the build's own cross compiler and the FLAT flags, then put
the result in the overlay:

```sh
../rv32_build/host/bin/riscv32-buildroot-linux-uclibc-gcc -O2 -fPIC \
    -Wl,-elf2flt=-r -Wl,-elf2flt=-s16384 -o foo foo.c a.c b.c
file foo                                   # must say BFLT
mkdir -p board/custom_rv32/overlay/usr/bin
cp foo board/custom_rv32/overlay/usr/bin/  # usr/bin/* is git-ignored
make -C ../rv32_build
```

`-s16384` is the stack size. This is quick for experiments, but a
package (recipe 1) rebuilds by itself and works for anyone who clones
the repo.

## Non-program files (scripts, config, data)

- **Copy as-is:** put them under `board/custom_rv32/overlay/` at their
  path in the rootfs, e.g. `overlay/etc/myapp.conf` → `/etc/myapp.conf`.
  For scripts, set the executable bit in the overlay (`chmod +x`) and use
  `#!/bin/sh`.
- **Shipped with a program:** install them from the package's
  `INSTALL_TARGET_CMDS`, e.g.
  `$(INSTALL) -D -m 0644 $(@D)/myapp.conf $(TARGET_DIR)/etc/myapp.conf`.
- **Run at boot:** add an init script `overlay/etc/init.d/S99myapp`
  (executable; busybox init runs `S??*` scripts in order with `start`).

## Rebuilding after changes

| You changed                         | Run (`make -C ../rv32_build ...`)        |
|-------------------------------------|------------------------------------------|
| Source files in `src/<name>/`       | `<name>-rebuild all`                     |
| `package/<name>/<name>.mk`          | `<name>-dirclean all`                    |
| `Config.in` / defconfig             | reload the defconfig (recipe 1, step 3), then `all` |
| Overlay files                       | `all`                                    |
| Removed a program or overlay file   | delete it from `../rv32_build/target/` too, then `all` |

- **`<name>-rebuild` is required for source edits.** `SITE_METHOD = local`
  copies (rsyncs) `src/<name>/` into `../rv32_build/build/<name>-1.0/` and
  builds there. A plain `make` won't notice the edit. Never edit the copy
  in `build/`, because the next rebuild overwrites it.
- **Deleted files stay in `target/`.** Buildroot never removes files from
  `target/` on its own, so a deleted program or overlay file stays in the
  image until you remove it there (or run `make clean all`).

## Checking the result

```sh
file ../rv32_build/target/usr/bin/<name>              # BFLT executable
../rv32_build/host/bin/riscv32-buildroot-linux-uclibc-flthdr \
    ../rv32_build/target/usr/bin/<name>               # stack size, RAM layout
../rv32_build/images/start-qemu.sh                    # then run it
```

In the guest, `cat /proc/<pid>/status` shows a `Mem:` line with the
program's RAM. `memhog` shows what's left for everything else.
