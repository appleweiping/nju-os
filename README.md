# NJU OS — Operating System Design & Implementation (Jyy, 2022)

> An independent, from-skeleton implementation of the **MiniLabs** and **OSLabs** of
> **NJU "操作系统：设计与实现" (Operating Systems: Design and Implementation)** by
> **蒋炎岩 (Yanyan Jiang / jyy)**, Nanjing University — part of a
> [csdiy.wiki](https://csdiy.wiki/) full-catalog build.

![status](https://img.shields.io/badge/status-9%2F9%20labs%20verified-brightgreen)
![language](https://img.shields.io/badge/C-informational)
![license](https://img.shields.io/badge/license-MIT-blue)

## Overview

This repo tackles the two lab tracks of jyy's OS course:

- **MiniLabs (M1–M5)** — five self-contained systems-programming exercises in C:
  a process-tree printer, a coroutine library, a syscall profiler, a C REPL, and a
  FAT32 file-recovery tool. Built as ordinary Linux programs (both 32- and 64-bit).
- **OSLabs (L1–L4)** — a small operating system built on jyy's
  [AbstractMachine](https://github.com/NJU-ProjectN/abstract-machine) hardware
  abstraction layer: a concurrent physical-memory allocator, a preemptive
  multithreading kernel, user processes, and a virtual file system.

Every implemented lab is **run and verified with real workloads**; measured results
are captured under [`results/`](results/).

## Results (measured on WSL2 Ubuntu 24.04, gcc 13.3, 16 threads)

| Lab | What it does | Result (measured) |
|---|---|---|
| **M1 pstree** | print the process tree from `/proc`, `-p`/`-n`/`-V` | node count == live process count; tree matches system `pstree` |
| **M2 libco** | stackful coroutines (`co_start`/`co_yield`/`co_wait`) | provided test suite passes on `-m64` **and** `-m32`: counters 0–199 & 200–399 in order |
| **M3 sperf** | aggregate syscall time via `strace -T -f` | on a `find` walk: getdents64/newfstatat/fcntl/close correctly dominate |
| **M4 crepl** | compile-and-run C expressions/functions via `dlopen` | `fib(20)=6765`, `sq(go(3,4))=49`, cross-`.so` recursion works |
| **M5 frecov** | recover deleted BMPs from a FAT32 image | 5/5 short-named byte-exact (live **and** post-delete); 2/2 long-named exact name+data |
| **L1 pmm** | concurrent buddy + per-CPU slab allocator | stress smp=1/4/8 → 2000 / 8000 / 16000 cycles, `FAIL=0` |
| **L2 kmt** | preemptive SMP scheduler, spinlocks, semaphores | mutex 8×50000=400000 exact; producer/consumer 4×20000=80000 exact; `PASSED` at smp=1/2/4/8 |
| **L3 uproc** | user processes: per-process address space (VME), user-mode contexts, `fork`/`exit`/`wait`/`exec`/`mmap`/`getpid`/`sleep`/`uptime`/`kputc` syscalls | `init` forks/reaps children with correct exit statuses, `exec`s `/bin/hello`, `mmap`s usable pages — **ALL TESTS PASSED** at smp=1/2/4 |
| **L4 vfs** | virtual file system: mount table + path resolution, inode/file abstractions, **ramfs** (/) + **devfs** (/dev) + **procfs** (/proc), wired under the L3 file syscalls | file round-trips via `open`/`write`/`lseek`/`read`/`close`; `/dev/{zero,null,random,tty}` and `/proc/{meminfo,uptime}` work; a child reads a file its parent created — **ALL TESTS PASSED** at smp=1/2/4 |

## Implemented labs

- [x] **M1 pstree** — reads `/proc/<pid>/stat`, rebuilds the hierarchy, prints an indented tree.
- [x] **M2 libco** — asymmetric stackful coroutines via `setjmp`/`longjmp` + an inline-asm stack switch (x86-64 `rsp` / i386 `esp`), round-robin scheduling.
- [x] **M3 sperf** — `pipe`+`fork`+`execvp("strace","-T","-f",...)`, parses the `<seconds>` trailer, aggregates per-syscall, prints a 1 Hz top.
- [x] **M4 crepl** — function defs → shared object + `dlopen(RTLD_GLOBAL)`; expressions → wrapped, compiled, `dlsym`'d and called; cross-snippet linking via the dynamic loader.
- [x] **M5 frecov** — raw 32-byte-aligned scan for FAT32 directory records, LFN name reconstruction, contiguous-cluster BMP recovery, inline SHA-1 (RFC 3174).
- [x] **L1 pmm** — buddy allocator (orders 0–16, coalescing) under a global spinlock + per-CPU slab caches (8–2048 B) for small objects; allocations aligned to the smallest power of two ≥ size; O(1) `free` via a page-descriptor table.
- [x] **L2 kmt** — preemptive round-robin scheduler driven by the timer IRQ and voluntary `yield` (through `os->trap`/`os->on_irq`); SMP spinlocks with xv6-style `push_off`/`pop_off`; sleeping counting semaphores; deferred kernel-stack release so no two CPUs ever run on the same stack during a switch.
- [x] **L3 uproc** — user processes on the AM VME: each process has its own `AddrSpace` (`protect`/`map`/`ucontext`) over L1 pmm pages; a system call is a trap-page access turned into `EVENT_SYSCALL`, dispatched through `os->trap`; the L2 kmt scheduler time-slices user processes. `fork` deep-copies the address space + trap frame, `wait` reaps zombies (blocking without busy-waiting via trap-RIP rewind), `exec` loads another program, `mmap` grows the user heap. User programs are freestanding flat binaries (`user/`) embedded into the kernel via `user/genprogs.py`.
- [x] **L4 vfs** — a mount table + path resolution (`namei`) over three real file systems: **ramfs** at `/` (read/write files & directories; holds `/bin` and `/tmp`), **devfs** at `/dev` (`zero`/`null`/`random`/`tty`), and **procfs** at `/proc` (`meminfo`/`uptime`/`self`, generated on read). Layered under the L3 `open`/`read`/`write`/`close`/`lseek`/`fstat`/`dup`/`mkdir`/`link`/`unlink`/`chdir` syscalls; `fork` dups the fd table, `exit` closes it, and `exec` loads program images out of `/bin`.

## Project structure

```
nju-os/
├── minilabs/                # M1–M5, ordinary Linux C programs
│   ├── pstree/  libco/  sperf/  crepl/  frecov/
│   └── Makefile  Makefile.lab
├── oslabs/                   # OSLabs on AbstractMachine
│   ├── abstract-machine/     # imported HAL framework (patched for glibc≥2.34)
│   ├── kernel/               # os.c, pmm.c (L1), kmt.c (L2), uproc.c (L3), vfs.c (L4)
│   ├── user/                 # freestanding user programs -> src/uprogs.inc
│   └── amgame/  Makefile.lab
├── results/                  # measured verification output per lab
└── LICENSE  README.md
```

## How to run

The MiniLabs build with a native toolchain (`gcc-multilib` for the 32-bit target).
The OSLabs run **fully headless** on AbstractMachine's **native** backend (no SDL /
display needed — the kernel uses the serial console and its own devfs).

```bash
sudo apt-get install -y gcc-multilib strace psmisc dosfstools mtools

# --- MiniLabs ---
cd minilabs/pstree && make all && ./pstree-64 -np          # M1
cd ../libco/tests  && make test                            # M2 (runs the suite, 64+32)
cd ../../sperf     && make all && ./sperf-64 find /usr/include -name '*.h'   # M3
cd ../crepl        && make all && ./crepl-64               # M4 (type C at the prompt)
cd ../frecov       && make all && bash tests/make_fat_test.sh                # M5

# --- OSLabs (AbstractMachine native) ---
cd oslabs/user   && make                                   # build user programs -> uprogs.inc
cd ../kernel
make ARCH=native TEST=pmm && smp=8 ./build/kernel-native   # L1 pmm stress
make ARCH=native TEST=kmt && smp=4 ./build/kernel-native   # L2 kmt self-test
make ARCH=native          && smp=1 ./build/kernel-native   # L3+L4: init runs the test suite
```

The L3/L4 build boots `init` (pid 1), which exercises the process/syscall interface
(L3) and then the file system (L4), printing an `[ok]`/`[FAIL]` line per check and
ending with `ALL TESTS PASSED`. Runs at `smp=1/2/4` (real fork-based SMP).

## Verification

Each lab's raw measured output lives under [`results/<lab>/verify.txt`](results/):

- **M1** node-count vs live process-count + a side-by-side with system `pstree`.
- **M2** the full provided test-suite output on both bit-widths, re-parsed to confirm
  the counter sequences are exactly `0..199` and `200..399` in order.
- **M3** the aggregated syscall profile of a directory walk.
- **M4** a scripted REPL session with expected/actual values.
- **M5** SHA-1 comparison of recovered BMPs against the originals (a generated FAT32
  image built with `mkfs.fat` + `mtools`; harness under `minilabs/frecov/tests/`).
- **L1** the `PMM STRESS RESULT` line at smp=1/4/8.
- **L2** the `KMT SELFTEST RESULT` line and a stability table across processor counts.
- **L3** the full `init` run — `getpid`/`uptime`/`fork`/`wait` with exit statuses,
  `exec` of a second program, and `mmap` — plus the pass line at smp=1/2/4.
- **L4** the same `init` run's file-system section — a file round-trip, `/dev/*`,
  `/proc/meminfo` — plus the pass line at smp=1/2/4.

### AbstractMachine notes

The `native` backend is the target for all four OSLabs and is patched only where a
modern toolchain / headless host requires it:

1. `platform.h` — `SIGSTKSZ` is no longer a compile-time constant in glibc ≥ 2.34,
   so the per-CPU signal stack is sized to a fixed `__AM_SIGSTKSZ`.
2. `framework/main.c` — `ioe_init()` is **not** called: on `native` it starts the
   SDL I/O stack whose event thread calls `halt()` on `SDL_QUIT`, which fires
   immediately on a headless host and would abort the kernel at a random time. The
   OS runs headless — console via `putch`/serial, timing via the timer IRQ, and a
   self-contained L4 devfs — so no AM I/O device is needed.

On `native`, "CPUs" are `fork`'d processes that share the kernel's memory via
`MAP_SHARED` (only the per-CPU `thiscpu` block is private), so the SMP tests above
exercise *real* shared-memory concurrency. For L3, a user process is a scheduler
task whose saved `Context` is a *user* context (`ucontext`) over an mmap-backed
`AddrSpace`; a system call is a read of the trap page (`0x100000`) that the AM CTE
delivers as `EVENT_SYSCALL`.

## Tech stack

C (gnu11), x86-64/i386, GNU Make; `setjmp`/`longjmp` + inline asm (coroutines);
`dlopen`/`dlsym` (crepl); FAT32 + inline SHA-1 (frecov); AbstractMachine HAL with
CTE (interrupts/context switch), VME (virtual memory: `protect`/`map`/`ucontext`)
and MPE (multiprocessing) for the OS kernel; freestanding flat-binary user programs.

## Key ideas / what I learned

- **Coroutines are just stacks + a jump.** `co_yield` is `setjmp` this stack +
  `longjmp` another; the only tricky part is bootstrapping a fresh stack with inline asm.
- **Incremental linking via the dynamic loader.** `crepl` never resolves symbols
  itself — it compiles each snippet to a `RTLD_GLOBAL` `.so` and lets `ld.so` bind
  cross-snippet calls, including recursion.
- **Deleted-file recovery** works because FAT only zeroes the directory entry's first
  byte and clears the FAT chain — the *data* clusters survive, so contiguous
  reconstruction + a header sanity check recovers the file byte-for-byte.
- **A concurrent allocator** wants per-CPU fast paths (slabs) to avoid lock traffic and
  a locked buddy allocator underneath for large/aligned blocks.
- **Preemptive SMP scheduling** hinges on two invariants: never preempt a CPU holding a
  spinlock (interrupt nesting), and never let two CPUs touch the same kernel stack
  (deferred stack release).
- **A user process is just a scheduler task with a user context.** Building it on the AM
  VME needs three pieces — an `AddrSpace` (`protect`/`map`), physical pages from the L1
  allocator, and a `ucontext` whose first entry drops into ring-3/user code; `fork` is a
  deep copy of that address space plus the trap frame with the child's `rax` set to 0.
- **Blocking syscalls without a per-process kernel thread.** Because the syscall handler
  runs in the trap and resumes *user* state, `wait`/`sleep` block by rewinding the trap
  RIP back onto the syscall instruction, so a woken process simply re-executes the call —
  no busy-waiting, no second stack.
- **A VFS is an indirection table.** `open` walks a mount table + path resolver to an
  inode; a file descriptor is `{inode, offset}`; ramfs, devfs and procfs differ only in
  what an inode's `read`/`write` does (a buffer, a device op, or generated text).

## Credits & license

Based on the labs of **NJU 操作系统：设计与实现** by **蒋炎岩 (Yanyan Jiang / jyy)**,
Nanjing University ([jyywiki.cn/OS/2022](https://jyywiki.cn/OS/2022/)). The lab skeletons
and the AbstractMachine framework are from
[github.com/NJU-ProjectN](https://github.com/NJU-ProjectN) and belong to their original
authors. This repository is an independent educational reimplementation; original code
here is released under the [MIT License](LICENSE).
